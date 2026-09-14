using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed class MediaCoreBridgeService : IMediaCoreBridge
{
    private readonly MediaCoreSupervisor _supervisor;
    private readonly object _gate = new();
    private Timer? _pollTimer;
    private long _pollTimerGeneration;
    // Leaf lock: guards only the poll timer + its generation. Never taken with _gate held.
    private readonly object _pollTimerGate = new();
    private Timer? _spineSyncTimer;
    private readonly SingleFlightTimerWork _pollWork = new();
    private readonly SingleFlightTimerWork _spineWork = new();
    private double _elapsedMs;
    private NativeMediaCoreStateSnapshot? _lastSnapshot;
    private Func<CancellationToken, Task<Dictionary<string, object?>>>? _spinePayloadFactory;
    private bool _spineSyncInFlight;
    private long _spineFactoryVersion;
    private CancellationTokenSource? _spineFactoryCancellation;

    public MediaCoreBridgeService(MediaCoreSupervisor? supervisor = null)
    {
        _supervisor = supervisor ?? new MediaCoreSupervisor();
        _supervisor.HealthChanged += health =>
        {
            if (health.Recovering)
            {
                // Do not let the periodic desired-state payload start a new recording
                // generation against color bars before Zoom has rejoined.
                StopSpineSync();
            }

            HealthChanged?.Invoke(health);
        };
        _supervisor.StatusChanged += status => StatusChanged?.Invoke(status);
        _supervisor.ProfileChanged += profile => ProfileChanged?.Invoke(profile);
        _supervisor.ZoomRecovered += PublishCaptureSnapshot;
        _supervisor.ZoomVideoFrameReceived += frame => ZoomVideoFrameReceived?.Invoke(frame);
        _supervisor.ProgramFramePreviewReceived += preview => ProgramFramePreviewReceived?.Invoke(preview);
        _supervisor.ProgramSharedTextureReceived += texture => ProgramSharedTextureReceived?.Invoke(texture);
        _supervisor.PreviewSharedTextureReceived += texture => PreviewSharedTextureReceived?.Invoke(texture);
        _supervisor.ParticipantSharedTextureReceived += texture => ParticipantSharedTextureReceived?.Invoke(texture);
        _supervisor.MultiviewSharedTextureReceived += texture => MultiviewSharedTextureReceived?.Invoke(texture);
    }

    public event Action<MediaCoreHealth>? HealthChanged;
    public event Action<string>? StatusChanged;
    public event Action<NativeMediaCoreProfile>? ProfileChanged;
    public event Action<NativeMediaCoreStateSnapshot>? SnapshotChanged;
    public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
    public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
    public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;
    public event Action<ProgramSharedTexture>? PreviewSharedTextureReceived;
    public event Action<ParticipantSharedTexture>? ParticipantSharedTextureReceived;
    public event Action<MultiviewSharedTexture>? MultiviewSharedTextureReceived;

    public MediaCoreHealth Health => _supervisor.Health;

    public NativeMediaCoreProfile? Profile => _supervisor.Profile;

    public string ProfileSummary => _supervisor.ProfileSummary;

    public bool Running => _supervisor.Running;

    public NativeMediaCoreStateSnapshot? LastSnapshot
    {
        get
        {
            lock (_gate)
            {
                return _lastSnapshot;
            }
        }
    }

    public void ConfigureProgramBufferFrames(int frames) => _supervisor.ConfigureProgramBufferFrames(frames);

    public async Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default)
    {
        var profile = await _supervisor.StartAsync(cancellationToken).ConfigureAwait(false);
        StartPolling();
        return profile;
    }

    public void Stop() => StopCore(() => _supervisor.Stop());

    /// <summary>App-exit stop only (T1.8, #461): close the core's stdin, give it
    /// <paramref name="exitGrace"/> to exit on its own, then kill the tree. See
    /// <see cref="MediaCoreSupervisor.StopForAppExit"/>. Leave-meeting and respawn use <see cref="Stop"/>.</summary>
    public MediaCoreExitOutcome StopForAppExit(TimeSpan exitGrace)
    {
        var outcome = MediaCoreExitOutcome.NotRunning;
        StopCore(() => outcome = _supervisor.StopForAppExit(exitGrace));
        return outcome;
    }

    private void StopCore(Action stopSupervisor)
    {
        StopPolling();
        ConfigureZoomSpineSync(null);
        stopSupervisor();
        lock (_gate)
        {
            _lastSnapshot = null;
            _elapsedMs = 0;
            _spinePayloadFactory = null;
            _spineFactoryVersion++;
        }
    }

    public void ConfigureZoomSpineSync(Func<CancellationToken, Task<Dictionary<string, object?>>>? payloadFactory)
    {
        lock (_gate) { _spinePayloadFactory = payloadFactory; _spineFactoryVersion++; }
        StopSpineSync();
        if (payloadFactory is not null && Running) StartSpineSync();
    }

    public MediaCoreSupervisor Supervisor => _supervisor;

    public Task<bool> PingAsync(CancellationToken cancellationToken = default) =>
        _supervisor.PingAsync(cancellationToken);

    public Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default) =>
        _supervisor.OpenVstEditorAsync(selection, cancellationToken);

    // A2: generic VST param bridge + state persistence passthroughs.
    public Task SetVstParamAsync(string selection, long paramId, double normalized,
        CancellationToken cancellationToken = default) =>
        _supervisor.SetVstParamAsync(selection, paramId, normalized, cancellationToken);

    public Task SetVstStateAsync(string selection, string stateBase64,
        CancellationToken cancellationToken = default) =>
        _supervisor.SetVstStateAsync(selection, stateBase64, cancellationToken);

    public Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default) =>
        _supervisor.GetVstStateAsync(selection, cancellationToken);

    public Task SetCaptureAudioSyncOffsetAsync(
        string deviceId,
        int offsetMs,
        CancellationToken cancellationToken = default) =>
        _supervisor.SetCaptureAudioSyncOffsetAsync(deviceId, offsetMs, cancellationToken);

    public Task RegisterCaptureShmAsync(
        string deviceId,
        string shmName,
        int width,
        int height,
        CancellationToken cancellationToken = default) =>
        _supervisor.RegisterCaptureShmAsync(deviceId, shmName, width, height, cancellationToken);

    /// <summary>
    /// Asks the core to open a capture device with its native (Media Foundation
    /// UVC) adapter. Returns the core's device states after the attempt; the
    /// caller decides success via NativeUvcCapturePolicy.FindConnectedDevice and
    /// falls back to the WinUI MediaCapture shared-memory bridge otherwise.
    /// </summary>
    public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectNativeCaptureDeviceAsync(
        string deviceId,
        CancellationToken cancellationToken = default,
        string? outputSourceId = null) =>
        _supervisor.ConnectCaptureDeviceAsync(deviceId, cancellationToken, outputSourceId);

    public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListNativeCaptureDevicesAsync(
        CancellationToken cancellationToken = default) =>
        _supervisor.ListCaptureDevicesAsync(cancellationToken);

    // Browser sources (BR-1): core-side WebView2 host processes; the sources show up
    // in ListNativeCaptureDevicesAsync as kind "browser" ids ("browser:<n>").
    public Task AddBrowserSourceAsync(
        string url, int width, int height, int fps, CancellationToken cancellationToken = default) =>
        _supervisor.AddBrowserSourceAsync(url, width, height, fps, cancellationToken);

    public Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
        _supervisor.RemoveBrowserSourceAsync(browserId, cancellationToken);

    public Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
        _supervisor.ReloadBrowserSourceAsync(browserId, cancellationToken);

    // Lifecycle L2: core-side session teardown for a capture device.
    public Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectNativeCaptureDeviceAsync(
        string deviceId,
        CancellationToken cancellationToken = default) =>
        _supervisor.DisconnectCaptureDeviceAsync(deviceId, cancellationToken);

    public async Task<RawCaptureSnapshot> JoinZoomAsync(
        string meetingUrl,
        string displayName,
        bool webinar,
        string? sdkJwt = null,
        string? userZak = null,
        CancellationToken cancellationToken = default,
        // #475: this join only. See MediaCoreSupervisor.JoinZoomAsync.
        bool endOtherMeeting = false)
    {
        if (!Running)
        {
            throw new InvalidOperationException("Media core is not running.");
        }

        var capture = await _supervisor.JoinZoomAsync(
                meetingUrl,
                displayName,
                webinar,
                sdkJwt,
                userZak,
                cancellationToken,
                endOtherMeeting)
            .ConfigureAwait(false);
        PublishCaptureSnapshot(capture);
        return capture;
    }

    public async Task<RawCaptureSnapshot> LeaveZoomAsync(CancellationToken cancellationToken = default)
    {
        if (!Running)
        {
            throw new InvalidOperationException("Media core is not running.");
        }

        var capture = await _supervisor.LeaveZoomAsync(cancellationToken).ConfigureAwait(false);
        PublishCaptureSnapshot(capture);
        return capture;
    }

    /// <summary>
    /// Capture-off: stop Zoom raw media in the engine while staying in the
    /// meeting (see <see cref="MediaCoreSupervisor.StopZoomCaptureAsync"/>).
    /// Publishing the snapshot keeps the merged meeting state fresh; the spine
    /// sync stays stopped because the payload factory is already null.
    /// </summary>
    public async Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default)
    {
        if (!Running)
        {
            throw new InvalidOperationException("Media core is not running.");
        }

        var capture = await _supervisor.StopZoomCaptureAsync(cancellationToken).ConfigureAwait(false);
        PublishCaptureSnapshot(capture);
        return capture;
    }

    public static string SummarizeJoinLeaveMessage(RawCaptureSnapshot snapshot, string verb) =>
        SummarizeCaptureSnapshot(snapshot, verb);

    public async Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default)
    {
        var capture = await _supervisor.GetZoomSnapshotAsync(cancellationToken).ConfigureAwait(false);
        PublishCaptureSnapshot(capture);
        return capture;
    }

    public async Task<NativeMediaCoreStateSnapshot> SyncAsync(
        IReadOnlyList<NativeMediaCoreCommand> commands,
        double? elapsedMs = null,
        CancellationToken cancellationToken = default)
    {
        lock (_gate)
        {
            if (elapsedMs is not null)
            {
                _elapsedMs = elapsedMs.Value;
            }
        }

        var snapshot = await _supervisor.SyncMediaCoreAsync(
            commands,
            GetElapsedMs(),
            cancellationToken).ConfigureAwait(false);
        PublishSnapshot(snapshot);
        return snapshot;
    }

    public async Task<NativeMediaCoreStateSnapshot> PollSnapshotAsync(
        CancellationToken cancellationToken = default)
    {
        AdvanceElapsed(16);
        return await SyncAsync([], cancellationToken: cancellationToken).ConfigureAwait(false);
    }

    public async Task<ZoomMediaSpineNativeSnapshot> SyncZoomMediaSpineAsync(
        Dictionary<string, object?> spinePayload,
        double? elapsedMs = null,
        CancellationToken cancellationToken = default)
    {
        if (!Running)
        {
            throw new InvalidOperationException("Media core is not running.");
        }

        lock (_gate)
        {
            if (elapsedMs is not null)
            {
                _elapsedMs = elapsedMs.Value;
            }
        }

        var spine = await _supervisor.SyncZoomMediaSpineAsync(
                spinePayload,
                GetElapsedMs(),
                cancellationToken)
            .ConfigureAwait(false);
        PublishSpineSnapshot(spine);
        return spine;
    }

    public static IReadOnlyList<NativeMediaCoreCommand> BuildSceneGraphCommand(
        string sceneId,
        IReadOnlyList<(string RouteId, string Mode, string AudioRole, string? ParticipantId)> routes)
    {
        return
        [
            new NativeMediaCoreCommand
            {
                Type = "load-scene-graph",
                ExtensionData = new Dictionary<string, System.Text.Json.JsonElement>
                {
                    ["sceneId"] = System.Text.Json.JsonSerializer.SerializeToElement(sceneId),
                    ["routes"] = System.Text.Json.JsonSerializer.SerializeToElement(
                        routes.Select(route => new
                        {
                            routeId = route.RouteId,
                            mode = route.Mode,
                            audioRole = route.AudioRole,
                            participantId = route.ParticipantId
                        }))
                }
            }
        ];
    }

    public static string SummarizeCaptureSnapshot(RawCaptureSnapshot snapshot, string verb)
    {
        if (!snapshot.MeetingState.Equals("in_meeting", StringComparison.OrdinalIgnoreCase))
        {
            var warning = snapshot.Warnings?.FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));
            if (!string.IsNullOrWhiteSpace(warning))
            {
                return $"{verb} failed — {warning}";
            }

            return $"{verb} — meeting {snapshot.MeetingState}.";
        }

        return $"{verb} — {snapshot.Participants.Count} participants in meeting.";
    }

    public static string SummarizeOutputs(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.Recording?.Lifecycle is not null)
        {
            return SummarizeLifecycleOutputs(snapshot);
        }
        var failedOutput = snapshot.OutputHealth
            .FirstOrDefault(item =>
                item.Status is "failed" or "warning" &&
                !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase) &&
                !string.IsNullOrWhiteSpace(item.Message));
        if (failedOutput is not null)
        {
            return $"{failedOutput.Destination.ToUpperInvariant()} output {failedOutput.Status}: {NormalizeOutputFailureMessage(failedOutput.Message)}";
        }

        var liveOutputs = snapshot.OutputHealth
            .Where(item =>
                item.Status is "live" &&
                !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase))
            .Select(item => item.Destination.ToUpperInvariant())
            .Distinct()
            .ToList();

        if (liveOutputs.Count > 0)
        {
            var live = $"Live: {string.Join(", ", liveOutputs)}";
            return snapshot.Recording?.Active == true ? $"Recording + {live}" : live;
        }

        var senderWarning = snapshot.OutputSenderSession.Warnings
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));
        if (!string.IsNullOrWhiteSpace(senderWarning))
        {
            return $"Output warning: {NormalizeOutputFailureMessage(senderWarning)}";
        }

        var failedSender = snapshot.OutputSenderSession.Senders
            .FirstOrDefault(static sender =>
                sender.Status is "failed" or "warning" &&
                (!string.IsNullOrWhiteSpace(sender.Warning) ||
                 !string.IsNullOrWhiteSpace(sender.LastError)));
        if (failedSender is not null)
        {
            var destination = string.IsNullOrWhiteSpace(failedSender.Destination)
                ? "output"
                : failedSender.Destination.ToUpperInvariant();
            return $"{destination} output {failedSender.Status}: {NormalizeOutputFailureMessage(failedSender.Warning ?? failedSender.LastError)}";
        }

        if (snapshot.Recording?.Active == true)
        {
            return $"Recording {snapshot.Recording.ProgramPath}";
        }

        var starting = snapshot.OutputSenderSession.Senders
            .Where(sender => sender.Status == "starting")
            .Select(sender => sender.Destination.ToUpperInvariant()).ToList();
        return starting.Count > 0 ? $"Starting: {string.Join(", ", starting)}" : "Outputs idle";
    }

    internal static string SummarizeLifecycleOutputs(NativeMediaCoreStateSnapshot snapshot)
    {
        // Recording lifecycle is always present in the new core, including when
        // only streaming. Compose independent outputs instead of letting idle
        // recording or one failed destination hide the remaining live output.
        var observations = snapshot.OutputHealth
            .Where(item => !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase))
            .Select(item => (Destination: item.Destination, Status: item.Status, Detail: (string?)item.Message))
            .Concat(snapshot.OutputSenderSession.Senders.Select(sender =>
                (Destination: sender.Destination, Status: sender.Status, Detail: sender.Warning ?? sender.LastError)))
            .GroupBy(item => item.Destination, StringComparer.OrdinalIgnoreCase);
        var parts = new List<string>();
        var live = new List<string>();
        var starting = new List<string>();
        foreach (var destination in observations)
        {
            var failure = destination.FirstOrDefault(item => item.Status == "failed");
            if (failure == default) failure = destination.FirstOrDefault(item => item.Status == "warning");
            if (failure != default)
            {
                var detail = string.IsNullOrWhiteSpace(failure.Detail)
                    ? destination.Select(item => item.Detail).FirstOrDefault(item => !string.IsNullOrWhiteSpace(item))
                    : failure.Detail;
                parts.Add($"{destination.Key.ToUpperInvariant()} output {failure.Status}" +
                    (string.IsNullOrWhiteSpace(detail) ? string.Empty : $": {NormalizeOutputFailureMessage(detail)}"));
            }
            else if (destination.Any(item => item.Status == "live")) live.Add(destination.Key.ToUpperInvariant());
            else if (destination.Any(item => item.Status == "starting")) starting.Add(destination.Key.ToUpperInvariant());
        }
        if (snapshot.Recording?.Lifecycle?.State != "idle")
            parts.Add(OutputLifecycleReadModel.RecordingStatus(snapshot.Recording));
        if (live.Count > 0) parts.Add($"Live: {string.Join(", ", live)}");
        if (starting.Count > 0) parts.Add($"Starting: {string.Join(", ", starting)}");
        var warning = snapshot.OutputSenderSession.Warnings.FirstOrDefault(item => !string.IsNullOrWhiteSpace(item));
        if (parts.Count == 0 && warning is not null) parts.Add($"Output warning: {NormalizeOutputFailureMessage(warning)}");
        return parts.Count > 0 ? string.Join(" · ", parts) : "Outputs idle";
    }

    private static string NormalizeOutputFailureMessage(string? message)
    {
        var detail = string.IsNullOrWhiteSpace(message)
            ? "No native output detail was returned."
            : message.Trim();

        string[] noisyPrefixes =
        [
            "media-core sync failed:",
            "native-media-core-sync failed:",
            "media-core request failed:",
            "start-program-output failed:",
            "start-program-output failed.",
            "program-output failed:",
            "program-output failed.",
            "output sender failed during sync:",
            "output sender failed:",
            "rtmp output sender failed:",
            "rtmp sender failed:"
        ];

        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var prefix in noisyPrefixes)
            {
                if (!detail.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                detail = detail[prefix.Length..].Trim();
                changed = true;
            }
        }

        return detail.Equals("missing:ffmpeg executable", StringComparison.OrdinalIgnoreCase)
            ? "FFmpeg executable was not found in the configured bin folder, app folder, or PATH."
            : detail;
    }

    // Start and stop are ATOMIC (one lock) so the installed timer always carries the runner's
    // current generation. Several startup edits reach StartAsync -> StartPolling at once
    // (EnsureMediaCoreRunningAsync is built to be called concurrently), and the unlocked version
    // interleaved: a stop reset the generation and disposed the newest timer, then an older start
    // still in flight installed ITS timer. That timer's generation was stale, so every tick was a
    // silent no-op and the shell's copy of core state froze for the whole session (meters, output
    // health, /snapshot) — seen live 2026-09-10 on beta-2026-09-10-5a24225 (dump: timer gen 17,
    // runner gen 20). A disposed timer's already-queued callback is harmless: its generation is stale.
    internal void StartPolling()
    {
        lock (_pollTimerGate)
        {
            _pollTimer?.Dispose();
            var generation = _pollWork.Reset();
            _pollTimerGeneration = generation;
            _pollTimer = new Timer(
                _ => _ = _pollWork.RunAsync(generation, PollLoopAsync),
                null,
                TimeSpan.FromMilliseconds(250),
                TimeSpan.FromMilliseconds(250));
        }

        StartSpineSync();
    }

    internal void StopPolling()
    {
        lock (_pollTimerGate)
        {
            _pollWork.Reset();
            _pollTimer?.Dispose();
            _pollTimer = null;
        }
    }

    private void StartSpineSync()
    {
        CancellationTokenSource? retired;
        lock (_gate)
        {
            if (_spineSyncTimer is not null && _spineFactoryCancellation is not null) return;
            _spineSyncTimer?.Dispose();
            _spineSyncTimer = null;
            retired = _spineFactoryCancellation;
            _spineFactoryCancellation = null;
            _spineFactoryVersion++;
            // Keep the desired factory through recovery, but create a fresh epoch
            // only while running. Timer creation and Stop share this same gate.
            if (_spinePayloadFactory is not null && Running)
            {
                _spineFactoryCancellation = new CancellationTokenSource();
                var generation = _spineWork.Reset();
                _spineSyncTimer = new Timer(_ => _ = _spineWork.RunAsync(generation, SpineSyncLoopAsync), null,
                    TimeSpan.FromMilliseconds(500), TimeSpan.FromMilliseconds(500));
            }
        }
        retired?.Cancel();
        retired?.Dispose();
    }

    private void StopSpineSync()
    {
        CancellationTokenSource? retired;
        lock (_gate)
        {
            // Retire the generation INSIDE the same lock StartSpineSync installs under, so stop is
            // one atomic step (the poll timer's start/stop race, #471). Outside it, a concurrent
            // start could take the "already running" path against a timer whose generation had
            // just gone stale.
            _spineWork.Reset();
            _spineSyncTimer?.Dispose();
            _spineSyncTimer = null;
            retired = _spineFactoryCancellation;
            _spineFactoryCancellation = null;
            _spineFactoryVersion++;
        }
        retired?.Cancel();
        retired?.Dispose();
    }

    /// <summary>Test probe: the installed poll timer was armed with the runner's CURRENT generation.
    /// When false, every tick is a silent no-op and the shell's copy of core state freezes.</summary>
    /// <summary>Test probe: stopped (no timer), or armed with the current generation — never a stale timer.</summary>
    internal bool PollTimerStateIsConsistent
    {
        get
        {
            lock (_pollTimerGate)
            {
                return _pollTimer is null || _pollTimerGeneration == _pollWork.CurrentGeneration;
            }
        }
    }

    internal bool PollTimerIsCurrent
    {
        get
        {
            lock (_pollTimerGate)
            {
                return _pollTimer is not null && _pollTimerGeneration == _pollWork.CurrentGeneration;
            }
        }
    }

    private async Task PollLoopAsync()
    {
        if (!Running)
        {
            return;
        }

        // T1.5 (#432): the core is polled on EVERY tick, Engine on or off. The capture-off
        // branch used to poll only the roster, which froze the shell's copy of core state
        // (/snapshot aged, frame counts stopped) until Engine On. See MediaCorePollPolicy.
        MediaCorePollPlan plan;
        lock (_gate)
        {
            plan = MediaCorePollPolicy.Plan(_spinePayloadFactory is not null, _lastSnapshot?.MeetingState);
        }

        // Core first, roster second: the roster merge keeps the fresh core fields it lands on,
        // so the state a tick leaves behind carries both. Each half is best-effort on its own,
        // so a poll skipped for backpressure never costs the roster refresh.
        if (plan.PollCoreSnapshot)
        {
            try
            {
                await PollSnapshotAsync().ConfigureAwait(false);
            }
            catch
            {
                // Polling is best-effort; supervisor health events surface hard failures.
            }
        }

        if (plan.RefreshZoomRoster)
        {
            try
            {
                await GetZoomSnapshotAsync().ConfigureAwait(false);
            }
            catch
            {
                // Same: the next tick retries.
            }
        }
    }

    private async Task SpineSyncLoopAsync()
    {
        Func<CancellationToken, Task<Dictionary<string, object?>>>? factory;
        long version;
        CancellationToken cancellationToken;
        lock (_gate)
        {
            if (_spineSyncInFlight || _spinePayloadFactory is null || _spineFactoryCancellation is null ||
                _lastSnapshot?.MeetingState != "in_meeting" || !Running) return;
            factory = _spinePayloadFactory;
            version = _spineFactoryVersion;
            cancellationToken = _spineFactoryCancellation!.Token;
            _spineSyncInFlight = true;
        }
        try
        {
            var payload = await factory(cancellationToken).WaitAsync(cancellationToken).ConfigureAwait(false);
            Task<ZoomMediaSpineNativeSnapshot> response;
            lock (_gate)
            {
                // Validate and submit as one ordered operation relative to Stop or
                // reconfiguration. No UI work or snapshot callbacks run under this lock.
                if (version != _spineFactoryVersion || !Running) return;
                _elapsedMs += 500;
                response = _supervisor.SyncZoomMediaSpineAsync(payload, _elapsedMs, cancellationToken);
            }
            var spine = await response.ConfigureAwait(false);
            lock (_gate) { if (version != _spineFactoryVersion) return; }
            PublishSpineSnapshot(spine);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
        catch (Exception error)
        {
            DiagnosticLog.WriteException("media-core.log", "spine sync failed", error);
        }
        finally
        {
            lock (_gate) { _spineSyncInFlight = false; }
        }
    }

    private void PublishSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        lock (_gate)
        {
            _lastSnapshot = snapshot;
        }

        SnapshotChanged?.Invoke(snapshot);
    }

    private void PublishCaptureSnapshot(RawCaptureSnapshot capture)
    {
        NativeMediaCoreStateSnapshot merged;
        lock (_gate)
        {
            merged = ZoomCaptureSnapshotMerger.Merge(_lastSnapshot, capture);
            _lastSnapshot = merged;
        }

        SnapshotChanged?.Invoke(merged);
        if (merged.MeetingState?.Equals("in_meeting", StringComparison.Ordinal) == true)
        {
            StartSpineSync();
        }
        else
        {
            StopSpineSync();
        }
    }

    private void PublishSpineSnapshot(ZoomMediaSpineNativeSnapshot spine)
    {
        NativeMediaCoreStateSnapshot merged;
        lock (_gate)
        {
            merged = ZoomMediaSpineSnapshotMerger.Merge(_lastSnapshot, spine);
            _lastSnapshot = merged;
        }

        SnapshotChanged?.Invoke(merged);
    }

    private double GetElapsedMs()
    {
        lock (_gate)
        {
            return _elapsedMs;
        }
    }

    private void AdvanceElapsed(double deltaMs)
    {
        lock (_gate)
        {
            _elapsedMs += deltaMs;
        }
    }

    public async ValueTask DisposeAsync()
    {
        StopPolling();
        ConfigureZoomSpineSync(null);
        await _supervisor.DisposeAsync().ConfigureAwait(false);
    }
}
