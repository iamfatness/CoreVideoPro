using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed class MediaCoreBridgeService : IAsyncDisposable
{
    private readonly MediaCoreSupervisor _supervisor;
    private readonly object _gate = new();
    private Timer? _pollTimer;
    private Timer? _spineSyncTimer;
    private double _elapsedMs;
    private NativeMediaCoreStateSnapshot? _lastSnapshot;
    private Func<Dictionary<string, object?>>? _spinePayloadFactory;
    private bool _spineSyncInFlight;

    public MediaCoreBridgeService(MediaCoreSupervisor? supervisor = null)
    {
        _supervisor = supervisor ?? new MediaCoreSupervisor();
        _supervisor.HealthChanged += health => HealthChanged?.Invoke(health);
        _supervisor.StatusChanged += status => StatusChanged?.Invoke(status);
        _supervisor.ProfileChanged += profile => ProfileChanged?.Invoke(profile);
        _supervisor.ZoomVideoFrameReceived += frame => ZoomVideoFrameReceived?.Invoke(frame);
        _supervisor.ProgramFramePreviewReceived += preview => ProgramFramePreviewReceived?.Invoke(preview);
        _supervisor.ProgramSharedTextureReceived += texture => ProgramSharedTextureReceived?.Invoke(texture);
    }

    public event Action<MediaCoreHealth>? HealthChanged;
    public event Action<string>? StatusChanged;
    public event Action<NativeMediaCoreProfile>? ProfileChanged;
    public event Action<NativeMediaCoreStateSnapshot>? SnapshotChanged;
    public event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
    public event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
    public event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;

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

    public async Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default)
    {
        var profile = await _supervisor.StartAsync(cancellationToken).ConfigureAwait(false);
        StartPolling();
        return profile;
    }

    public void Stop()
    {
        StopPolling();
        StopSpineSync();
        _supervisor.Stop();
        lock (_gate)
        {
            _lastSnapshot = null;
            _elapsedMs = 0;
            _spinePayloadFactory = null;
        }
    }

    public void ConfigureZoomSpineSync(Func<Dictionary<string, object?>>? payloadFactory)
    {
        lock (_gate)
        {
            _spinePayloadFactory = payloadFactory;
        }

        if (payloadFactory is null)
        {
            StopSpineSync();
            return;
        }

        if (Running)
        {
            StartSpineSync();
        }
    }

    public MediaCoreSupervisor Supervisor => _supervisor;

    public Task<bool> PingAsync(CancellationToken cancellationToken = default) =>
        _supervisor.PingAsync(cancellationToken);

    public async Task<RawCaptureSnapshot> JoinZoomAsync(
        string meetingUrl,
        string displayName,
        bool webinar,
        string? sdkJwt = null,
        string? userZak = null,
        CancellationToken cancellationToken = default)
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
                cancellationToken)
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
        if (snapshot.Recording?.Active == true)
        {
            return $"Recording {snapshot.Recording.ProgramPath}";
        }

        var liveOutputs = snapshot.OutputHealth
            .Where(item => item.Status is "live" or "warning")
            .Select(item => item.Destination.ToUpperInvariant())
            .Distinct()
            .ToList();

        return liveOutputs.Count > 0
            ? $"Live: {string.Join(", ", liveOutputs)}"
            : "Outputs idle";
    }

    private void StartPolling()
    {
        StopPolling();
        _pollTimer = new Timer(
            _ => _ = PollLoopAsync(),
            null,
            TimeSpan.FromMilliseconds(250),
            TimeSpan.FromMilliseconds(250));
        StartSpineSync();
    }

    private void StopPolling()
    {
        _pollTimer?.Dispose();
        _pollTimer = null;
    }

    private void StartSpineSync()
    {
        StopSpineSync();
        lock (_gate)
        {
            if (_spinePayloadFactory is null)
            {
                return;
            }
        }

        _spineSyncTimer = new Timer(
            _ => _ = SpineSyncLoopAsync(),
            null,
            TimeSpan.FromMilliseconds(500),
            TimeSpan.FromMilliseconds(500));
    }

    private void StopSpineSync()
    {
        _spineSyncTimer?.Dispose();
        _spineSyncTimer = null;
    }

    private async Task PollLoopAsync()
    {
        if (!Running)
        {
            return;
        }

        try
        {
            if (ShouldRefreshZoomRosterWhileCaptureOff())
            {
                await GetZoomSnapshotAsync().ConfigureAwait(false);
                return;
            }

            await PollSnapshotAsync().ConfigureAwait(false);
        }
        catch
        {
            // Polling is best-effort; supervisor health events surface hard failures.
        }
    }

    private bool ShouldRefreshZoomRosterWhileCaptureOff()
    {
        lock (_gate)
        {
            if (_spinePayloadFactory is not null)
            {
                return false;
            }

            var meetingState = ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(_lastSnapshot?.MeetingState);
            return meetingState.Equals("in_meeting", StringComparison.Ordinal);
        }
    }

    private async Task SpineSyncLoopAsync()
    {
        if (!Running || _spineSyncInFlight)
        {
            return;
        }

        Func<Dictionary<string, object?>>? factory;
        NativeMediaCoreStateSnapshot? snapshot;
        lock (_gate)
        {
            factory = _spinePayloadFactory;
            snapshot = _lastSnapshot;
        }

        if (factory is null ||
            snapshot?.MeetingState is not { } meetingState ||
            !meetingState.Equals("in_meeting", StringComparison.Ordinal))
        {
            return;
        }

        _spineSyncInFlight = true;
        try
        {
            AdvanceElapsed(500);
            await SyncZoomMediaSpineAsync(factory(), cancellationToken: CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // Spine sync is best-effort while the meeting is live.
        }
        finally
        {
            _spineSyncInFlight = false;
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
        StopSpineSync();
        await _supervisor.DisposeAsync().ConfigureAwait(false);
    }
}