using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed class MediaCoreBridgeService : IAsyncDisposable
{
    private readonly MediaCoreSupervisor _supervisor;
    private readonly object _gate = new();
    private Timer? _pollTimer;
    private double _elapsedMs;
    private NativeMediaCoreStateSnapshot? _lastSnapshot;

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
        _supervisor.Stop();
        lock (_gate)
        {
            _lastSnapshot = null;
            _elapsedMs = 0;
        }
    }

    public MediaCoreSupervisor Supervisor => _supervisor;

    public Task<bool> PingAsync(CancellationToken cancellationToken = default) =>
        _supervisor.PingAsync(cancellationToken);

    public async Task<RawCaptureSnapshot> JoinZoomAsync(
        string meetingUrl,
        string displayName,
        bool webinar,
        CancellationToken cancellationToken = default)
    {
        if (!Running)
        {
            throw new InvalidOperationException("Media core is not running.");
        }

        return await _supervisor.JoinZoomAsync(meetingUrl, displayName, webinar, cancellationToken)
            .ConfigureAwait(false);
    }

    public async Task<RawCaptureSnapshot> LeaveZoomAsync(CancellationToken cancellationToken = default)
    {
        if (!Running)
        {
            throw new InvalidOperationException("Media core is not running.");
        }

        return await _supervisor.LeaveZoomAsync(cancellationToken).ConfigureAwait(false);
    }

    public static string SummarizeJoinLeaveMessage(RawCaptureSnapshot snapshot, string verb) =>
        SummarizeCaptureSnapshot(snapshot, verb);

    public Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default) =>
        _supervisor.GetZoomSnapshotAsync(cancellationToken);

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
        if (!snapshot.MeetingState.Equals("in_meeting", StringComparison.Ordinal))
        {
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
    }

    private void StopPolling()
    {
        _pollTimer?.Dispose();
        _pollTimer = null;
    }

    private async Task PollLoopAsync()
    {
        if (!Running)
        {
            return;
        }

        try
        {
            await PollSnapshotAsync().ConfigureAwait(false);
        }
        catch
        {
            // Polling is best-effort; supervisor health events surface hard failures.
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
        await _supervisor.DisposeAsync().ConfigureAwait(false);
    }
}