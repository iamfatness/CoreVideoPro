using CoreVideoPro.WinUI.Services;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels.Transport;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Characterization tests for <see cref="TransportCoordinator"/> — the record / stream / take /
/// engine orchestration extracted from the StudioViewModel god object (PR2 strangler). These are
/// the FIRST-EVER coverage of transport orchestration: they exist because the coordinator is
/// constructible in isolation (fake <see cref="IMediaCoreBridge"/> + fake <see cref="ITransportHost"/>
/// + inline <see cref="ITransportDispatcher"/>), which StudioViewModel itself is not. They lock the
/// golden-path behavior — arm, in-flight guard, #286-class rollback, spine-callback install, Take
/// promotion — BEFORE anyone refactors it further.
/// </summary>
public sealed class TransportCoordinatorTests
{
    private static (TransportCoordinator Coordinator, FakeMediaCoreBridge Bridge, FakeTransportHost Host) Build(
        int recordingSyncRetryAttempts = 120,
        int recordingSyncRetryDelayMs = 250)
    {
        var bridge = new FakeMediaCoreBridge();
        var host = new FakeTransportHost();
        var coordinator = new TransportCoordinator(
            bridge,
            host,
            host,
            recordingSyncRetryAttempts,
            recordingSyncRetryDelayMs);
        return (coordinator, bridge, host);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExplicitStop_RetriesFailedStopWithoutRearming(bool recording)
    {
        var (coordinator, _, host) = Build();
        host.Recording = recording;
        host.Streaming = !recording;
        host.SyncThrows = new InvalidOperationException("stop did not reach core");
        Func<bool, Task> set = recording ? coordinator.SetRecordingAsync : coordinator.SetStreamingAsync;
        await set(false);
        Assert.False(recording ? host.Recording : host.Streaming);
        Assert.Equal(1, host.SyncCallCount);

        host.SyncThrows = null;
        host.HoldSync = true;
        var retry = StudioControlSurface.RunOutputSet(false, true, false, _ => true, set, "output");
        Assert.Equal(2, host.SyncCallCount);
        Assert.False(recording ? host.Recording : host.Streaming);
        host.ReleaseSync();
        await retry;
        Assert.False(recording ? host.Recording : host.Streaming);
        Assert.Contains(recording ? "Recording stop requested" : "Streaming stopped", host.OutputStatus);
    }

    [Theory]
    [InlineData(false, false, false, 0)]
    [InlineData(true, false, true, 0)]
    [InlineData(false, true, false, 1)]
    [InlineData(true, true, false, 1)]
    [InlineData(false, false, true, 1)]
    public async Task ExplicitOutputSet_PreservesIdempotencyAndPassesTarget(bool requested, bool live, bool target, int expectedCalls)
    {
        var calls = new List<bool>();
        await StudioControlSurface.RunOutputSet(requested, live, target, _ => true,
            value => { calls.Add(value); return Task.CompletedTask; }, "output");
        Assert.Equal(expectedCalls, calls.Count);
        Assert.All(calls, value => Assert.Equal(target, value));
    }

    [Fact]
    public async Task ExplicitOutputSet_DoesNotRunWhileUnavailable()
    {
        var called = false;
        await StudioControlSurface.RunOutputSet(false, true, false, _ => false,
            _ => { called = true; return Task.CompletedTask; }, "output");
        Assert.False(called);
    }

    // ---------------------------------------------------------------- Recording

    [Fact]
    public async Task ToggleRecording_ArmsViaBridgeSync_AndSetsInFlightDuringTheToggle()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        host.HoldSync = true; // keep the arming sync pending so the toggle stays in-flight

        var toggle = coordinator.ToggleRecordingAsync();

        // Mid-flight: recording armed (host.Recording flipped true) and the guard is set.
        Assert.True(host.Recording);
        Assert.True(coordinator.RecordingToggleInFlight);
        Assert.Equal(1, host.SyncCallCount);

        host.ReleaseSync();
        await toggle;

        Assert.True(host.Recording);
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.Contains("Recording start requested.", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_DoubleToggle_IsGuardedByTheInFlightFlag()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        host.HoldSync = true;

        var first = coordinator.ToggleRecordingAsync();       // arms, then blocks on the sync gate
        var second = coordinator.ToggleRecordingAsync();       // must early-return on the in-flight guard
        await second;                                          // second returns immediately

        Assert.Equal(1, host.SyncCallCount);                   // only the first toggle reached the sync

        host.ReleaseSync();
        await first;
        Assert.False(coordinator.RecordingToggleInFlight);
    }

    [Fact]
    public async Task ToggleRecording_RollsBackAndClearsInFlight_WhenSyncThrows()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        host.SyncThrows = new InvalidOperationException("media core rejected the recording request");

        await coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);                          // rolled back to the previous state
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.StartsWith("Recording start failed:", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_RollsBack_OnHealthFailureSnapshot_286Class()
    {
        var (coordinator, _, host) = Build();
        host.Recording = false;
        // Sync SUCCEEDS but the snapshot reports a failed recording output (the #286 shape:
        // a video-only / broken recording must never look healthy).
        host.SyncResult = new NativeMediaCoreStateSnapshot
        {
            OutputHealth =
            [
                new NativeMediaCoreOutputHealth
                {
                    Destination = "recording",
                    Status = "failed",
                    Message = "recorder could not open the target file"
                }
            ]
        };

        await coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);                          // health-proof rollback
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.StartsWith("Recording start failed:", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_StopWaitsForBusyStartAndKeepsCommandGuarded()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 6, recordingSyncRetryDelayMs: 1);
        host.Recording = true;
        host.SyncFailuresRemaining = 3;

        var toggle = coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);                           // stop intent is sticky immediately
        Assert.True(coordinator.RecordingToggleInFlight);       // second click cannot re-arm

        await toggle;

        Assert.False(host.Recording);
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.Equal(4, host.SyncCallCount);
        Assert.Equal("Recording stop requested — finalizing.", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleRecording_StopRetryExhaustionNeverRearmsRecording()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 2, recordingSyncRetryDelayMs: 1);
        host.Recording = true;
        host.SyncFailuresRemaining = int.MaxValue;

        await coordinator.ToggleRecordingAsync();

        Assert.False(host.Recording);
        Assert.False(coordinator.RecordingToggleInFlight);
        Assert.Equal(3, host.SyncCallCount);                    // initial attempt plus two retries
        Assert.Contains("stop remains armed", host.OutputStatus, StringComparison.OrdinalIgnoreCase);
    }

    // ---------------------------------------------------------------- Streaming

    [Fact]
    public async Task ToggleStreaming_FailedStopKeepsDesiredStateDisarmed()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = true;
        host.SyncThrows = new InvalidOperationException("connection lost during stop");

        await coordinator.ToggleStreamingAsync();

        Assert.False(host.Streaming);
        Assert.StartsWith("Streaming stop failed:", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleStreaming_ArmsAndProvesStart_WhenSenderGoesLive()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamDestinations = ["rtmp://live.example/app/key"];
        host.SyncResult = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new NativeMediaCoreOutputSenderSession
            {
                Status = "live",
                ActiveSenderCount = 1,
                Senders =
                [
                    new NativeMediaCoreOutputSender
                    {
                        SenderId = "s1",
                        Destination = "rtmp://live.example/app/key",
                        Status = "live"
                    }
                ]
            }
        };

        await coordinator.ToggleStreamingAsync();

        Assert.True(host.Streaming);
        Assert.False(coordinator.StreamToggleInFlight);
        Assert.Contains("Streaming start requested.", host.OutputStatus);
    }

    [Fact]
    public async Task ToggleStreaming_BlockedBeforeArming_WhenDestinationsInvalid()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamValidationError = "Select at least one stream destination before streaming.";

        await coordinator.ToggleStreamingAsync();

        Assert.False(host.Streaming);                          // never armed
        Assert.Equal(0, host.SyncCallCount);                   // no core round-trip
        Assert.StartsWith("Streaming start failed:", host.OutputStatus);
        Assert.False(coordinator.StreamToggleInFlight);
    }

    [Fact]
    public async Task ToggleStreaming_DoubleToggle_IsGuardedByTheInFlightFlag()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamDestinations = ["rtmp://live.example/app/key"];
        host.HoldSync = true;

        var first = coordinator.ToggleStreamingAsync();
        var second = coordinator.ToggleStreamingAsync();
        await second;

        Assert.Equal(1, host.SyncCallCount);

        host.ReleaseSync();
        await first;
        Assert.False(coordinator.StreamToggleInFlight);
    }

    [Fact]
    public async Task ToggleStreaming_RollsBackAndClearsInFlight_WhenSyncThrows()
    {
        var (coordinator, _, host) = Build();
        host.Streaming = false;
        host.StreamDestinations = ["rtmp://live.example/app/key"];
        host.SyncThrows = new InvalidOperationException("rtmp output sender failed: connection refused");

        await coordinator.ToggleStreamingAsync();

        Assert.False(host.Streaming);                          // rolled back
        Assert.False(coordinator.StreamToggleInFlight);
        Assert.StartsWith("Streaming start failed:", host.OutputStatus);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("deleted-scene")]
    public async Task Take_InvalidPreviewCannotPoisonProgram(string? preview)
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = preview!;
        await coordinator.TakeAsync();
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal(0, host.SyncCallCount);
        Assert.Equal(0, host.PromoteCallCount);
        Assert.Contains("unavailable", host.CommandStatus);
    }

    [Fact]
    public async Task Take_BusySyncRetriesSameProgramWithoutSwappingAgain()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 3, recordingSyncRetryDelayMs: 1);
        host.PreviewSceneId = "interview";
        host.SyncFailuresRemaining = 2;
        await coordinator.TakeAsync();
        Assert.Equal(new[] { "interview", "interview", "interview" }, host.SyncedProgramIds);
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal(1, host.TakeVersionIncrements);
        Assert.Equal(0, host.RollbackCount);
        Assert.Equal("Program updated", host.OutputStatus);
    }

    [Fact]
    public async Task Take_DoubleInvocationWhileAwaitingSyncDoesNotSwapBack()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        host.HoldSync = true;
        var first = coordinator.TakeAsync();
        await coordinator.TakeAsync();
        Assert.Equal("interview", host.ActiveSceneId);
        Assert.Equal(1, host.SyncCallCount);
        host.ReleaseSync();
        await first;
    }

    [Fact]
    public async Task Take_FailureRestoresPriorProgramAndRetainsPreviewForRetry()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        host.SyncThrows = new InvalidOperationException("native rejected scene");
        await coordinator.TakeAsync();
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(1, host.RollbackCount);
        Assert.Contains("previous local Program restored", host.CommandStatus);
        host.SyncThrows = null;
        await coordinator.TakeAsync();
        Assert.Equal("interview", host.ActiveSceneId);
    }

    [Fact]
    public async Task Take_ExhaustedBackpressureRestoresProgramInsteadOfClaimingSuccess()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 2, recordingSyncRetryDelayMs: 1);
        host.PreviewSceneId = "interview";
        host.SyncFailuresRemaining = 10;
        await coordinator.TakeAsync();
        Assert.Equal(2, host.SyncCallCount);
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(1, host.RollbackCount);
        Assert.NotEqual("Program updated", host.OutputStatus);
    }

    [Fact]
    public async Task TakeApi_DisabledControlFailsWithoutInvokingTake()
    {
        var invoked = false;
        var result = await StudioControlSurface.RunTake(false, () =>
        {
            invoked = true;
            return Task.FromResult(TakeResult.Success);
        });
        Assert.False(result.Ok);
        Assert.False(invoked);
        Assert.Contains("unavailable", result.Error);
    }

    [Fact]
    public async Task TakeApi_ReportsEachInvocationOutcomeInsteadOfPriorSuccess()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        var success = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.True(success.Ok);
        host.SyncThrows = new InvalidOperationException("core rejected next scene");
        var failure = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(failure.Ok);
        Assert.Contains("core rejected next scene", failure.Error);
        Assert.Contains("not confirmed", failure.Error);
    }

    [Fact]
    public async Task TakeApi_ConcurrentInvocationCannotBorrowFirstCompletion()
    {
        var (coordinator, _, host) = Build();
        host.PreviewSceneId = "interview";
        host.HoldSync = true;
        var first = StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        var duplicate = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(duplicate.Ok);
        Assert.Contains("already in progress", duplicate.Error);
        Assert.False(first.IsCompleted);
        host.ReleaseSync();
        Assert.True((await first).Ok);
        Assert.Equal(1, host.SyncCallCount);
    }

    [Fact]
    public async Task TakeApi_OfflineLocalSelectionIsNotReportedAsOnAirSuccess()
    {
        var (coordinator, bridge, host) = Build();
        bridge.Running = false;
        host.PreviewSceneId = "interview";
        var result = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(result.Ok);
        Assert.Contains("offline", result.Error);
        Assert.Equal(0, host.SyncCallCount);
    }

    [Fact]
    public async Task TakeApi_ExhaustedBusySyncFails()
    {
        var (coordinator, _, host) = Build(recordingSyncRetryAttempts: 1);
        host.PreviewSceneId = "interview";
        host.SyncFailuresRemaining = 1;
        var result = await StudioControlSurface.RunTake(true, coordinator.TakeAsync);
        Assert.False(result.Ok);
        Assert.Contains("busy", result.Error);
        Assert.Equal("intro", host.ActiveSceneId);
    }

    [Fact]
    public async Task TakeApi_UnexpectedCommandExceptionReturnsFailure()
    {
        var result = await StudioControlSurface.RunTake(true, () => throw new InvalidOperationException("route preparation failed"));
        Assert.False(result.Ok);
        Assert.Contains("route preparation failed", result.Error);
    }

    // ---------------------------------------------------------------- Take

    [Fact]
    public async Task Take_PromotesPreviewToProgram_AndSwapsProgramBackToPreview()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "interview";
        host.HasPendingCue = false;

        await coordinator.TakeAsync();

        Assert.Equal("interview", host.ActiveSceneId);          // preview promoted to program
        Assert.Equal("intro", host.PreviewSceneId);             // old program swapped back to preview
        Assert.Equal(1, host.PromoteCallCount);
        Assert.Equal(1, host.TakeVersionIncrements);
        Assert.Equal("Program updated", host.OutputStatus);
        Assert.Equal(1, host.SyncCallCount);
    }

    [Fact]
    public async Task Take_CommitsPendingDraft_WhenPreviewAndProgramShareTheScene()
    {
        var (coordinator, _, host) = Build();
        host.ActiveSceneId = "intro";
        host.PreviewSceneId = "intro";                          // same scene on PGM + PVW
        host.HasPendingCue = true;                              // but a media cue is drafted

        await coordinator.TakeAsync();

        Assert.Equal(1, host.CopyPreviewRoutesCallCount);       // draft committed in place
        Assert.Equal("intro", host.ActiveSceneId);              // no scene swap
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal(1, host.PromoteCallCount);
    }

    // ---------------------------------------------------------------- Engine

    [Fact]
    public async Task ToggleEngine_InstallsSpineCallback_AndSubscribesZoomCapture()
    {
        var (coordinator, bridge, host) = Build();
        host.ZoomCaptureSubscribed = false;
        bridge.Running = true;

        await coordinator.ToggleEngineAsync();

        Assert.True(bridge.SpineCallbackInstalled);             // ConfigureZoomSpineSync(BuildSpinePayload)
        Assert.True(host.ZoomCaptureSubscribed);
        Assert.True(host.SurfacesCaptureSubscribed);
    }

    [Fact]
    public async Task ToggleEngine_UnsubscribesZoomCapture_WhenAlreadySubscribed()
    {
        var (coordinator, _, host) = Build();
        host.ZoomCaptureSubscribed = true;

        await coordinator.ToggleEngineAsync();

        Assert.Equal(1, host.UnsubscribeCallCount);
    }

    [Fact]
    public async Task ToggleEngine_ReportsUnavailable_WhenCoreDoesNotComeUp()
    {
        var (coordinator, bridge, host) = Build();
        host.ZoomCaptureSubscribed = false;
        bridge.Running = false; // EnsureMediaCoreRunningAsync is a no-op fake; core stays down

        await coordinator.ToggleEngineAsync();

        Assert.False(host.ZoomCaptureSubscribed);
        Assert.False(bridge.SpineCallbackInstalled);
    }

    // ================================================================ fakes

    private sealed class FakeTransportHost : ITransportHost, ITransportDispatcher
    {
        private TaskCompletionSource<bool>? _syncGate;

        public bool Recording { get; set; }

        public bool Streaming { get; set; }

        public bool ZoomCaptureSubscribed { get; set; }

        public string EngineStatus { private get; set; } = string.Empty;

        public string CommandStatus { get; set; } = string.Empty;

        public string OutputStatus { get; set; } = "Outputs idle";

        public string OutputSessionStatus { private get; set; } = string.Empty;

        public string? RecordingDiskWarning { private get; set; }

        public string? SelectedParticipantId => null;

        public string ActiveSceneId { get; set; } = "intro";

        public string PreviewSceneId { get; set; } = "intro";

        public string ProgramSceneSummary => ActiveSceneId;

        public string TakeTransitionLabel => "Fade";

        public bool IsSceneAvailable(string? sceneId) => sceneId is "intro" or "interview";

        // --- test knobs ---
        public bool HoldSync { get; set; }

        public Exception? SyncThrows { get; set; }

        public int SyncFailuresRemaining { get; set; }

        public NativeMediaCoreStateSnapshot SyncResult { get; set; } = new();

        public bool HasPendingCue { get; set; }

        public IReadOnlyList<string> StreamDestinations { get; set; } = ["rtmp://live.example/app/key"];

        public string? StreamValidationError { get; set; }

        // --- observed counters ---
        public int SyncCallCount { get; private set; }
        public List<string> SyncedProgramIds { get; } = [];

        public int PromoteCallCount { get; private set; }

        public int TakeVersionIncrements { get; private set; }

        public int CopyPreviewRoutesCallCount { get; private set; }

        public int RollbackCount { get; private set; }

        public int UnsubscribeCallCount { get; private set; }

        public bool SurfacesCaptureSubscribed { get; private set; }

        public void ReleaseSync() => _syncGate?.TrySetResult(true);

        // --- ITransportDispatcher: run inline (tests assert on the marshalled writes) ---
        public void RunOnUiThread(Action action) => action();

        // --- ITransportHost ---
        public bool HasPendingPreviewChanges(string sceneId) => HasPendingCue;

        public Func<Func<bool>> CaptureTakeRollback()
        {
            var program = ActiveSceneId;
            var preview = PreviewSceneId;
            return () => () => { ActiveSceneId = program; PreviewSceneId = preview; RollbackCount++; return true; };
        }

        public void BeginTakeMutation() { }
        public void EndTakeMutation() { }
        public void RequestTakeReconciliation() { }

        public void CopyPreviewRoutesToScene(string sceneId) => CopyPreviewRoutesCallCount++;

        public void PromoteProgramMediaRouteToPlayback() => PromoteCallCount++;

        public void RefreshPreviewRoutingState() { }

        public void IncrementProgramMediaPlaybackTakeVersion() => TakeVersionIncrements++;

        public Task EnsureMediaCoreRunningAsync(string startingStatus) => Task.CompletedTask;

        public async Task<NativeMediaCoreStateSnapshot> SyncActiveSceneAsync(string? reason = null)
        {
            SyncCallCount++;
            SyncedProgramIds.Add(ActiveSceneId);
            if (SyncFailuresRemaining > 0)
            {
                SyncFailuresRemaining--;
                throw new MediaCoreSyncInFlightException();
            }

            if (HoldSync)
            {
                _syncGate = new TaskCompletionSource<bool>();
                await _syncGate.Task;
            }

            if (SyncThrows is not null)
            {
                throw SyncThrows;
            }

            return SyncResult;
        }

        public Dictionary<string, object?> BuildSpinePayload() => new() { ["spine"] = true };

        public void UnsubscribeZoomCapture(string status)
        {
            UnsubscribeCallCount++;
            ZoomCaptureSubscribed = false;
        }

        public void NotifySurfacesCaptureSubscribed(bool subscribed, string? compositorRenderer) =>
            SurfacesCaptureSubscribed = subscribed;

        public void NotifySurfacesPreviewParticipant(string? participantId) { }

        public void RefreshSdkReadiness() { }

        public void RefreshSurfaceBindings() { }

        public void RefreshTransportState() { }

        public void RefreshOutputStatus() { }

        public bool TryEvaluateRecordingDiskPreflight(out IsoDiskPreflightResult result)
        {
            result = null!;
            return false;
        }

        public IReadOnlyList<string> BuildSelectedStreamDestinations(bool validatedOnly) => StreamDestinations;

        public string? ValidateStreamDestinations() => StreamValidationError;

        public void NotifyRecordingCommandCanExecuteChanged() { }

        public void NotifyStreamingCommandCanExecuteChanged() { }

        public string RecordingLogFormat => "mp4";

        public double RecordingLogBitrateMbps => 24;

        public string RecordingVideoCodec => "h264";

        public bool StreamRtmpEnabled => true;

        public bool StreamNdiEnabled => false;

        public bool StreamSrtEnabled => false;

        public double StreamLogBitrateMbps => 8;

        public string StreamVideoCodec => "h264";

        public string StreamEncoderMode => "auto";

        public string FormatStreamDestinationTelemetry(IReadOnlyList<string> destinations) =>
            string.Join(",", destinations);
    }

    private sealed class FakeMediaCoreBridge : IMediaCoreBridge
    {
        public bool Running { get; set; } = true;

        public bool SpineCallbackInstalled { get; private set; }

        public NativeMediaCoreProfile? Profile => null;

        public string ProfileSummary => "GPU 1080p60";

        public NativeMediaCoreStateSnapshot? LastSnapshot => null;

        public NativeMediaCoreStateSnapshot PollResult { get; set; } = new();

#pragma warning disable CS0067 // events are part of the seam surface; the coordinator does not raise them
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
#pragma warning restore CS0067

        public void ConfigureZoomSpineSync(Func<Dictionary<string, object?>>? payloadFactory) =>
            SpineCallbackInstalled = payloadFactory is not null;

        public Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult<NativeMediaCoreProfile?>(null);

        public void Stop() { }

        public Task<NativeMediaCoreStateSnapshot> PollSnapshotAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(PollResult);

        // --- unused by the coordinator (the seam surface StudioViewModel wires elsewhere) ---
        public Task<NativeMediaCoreStateSnapshot> SyncAsync(
            IReadOnlyList<NativeMediaCoreCommand> commands, double? elapsedMs = null,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetVstParamAsync(string selection, long paramId, double normalized,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task SetVstStateAsync(string selection, string stateBase64,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task SetCaptureAudioSyncOffsetAsync(string deviceId, int offsetMs,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RegisterCaptureShmAsync(string deviceId, string shmName, int width, int height,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default, string? outputSourceId = null) =>
            throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListNativeCaptureDevicesAsync(
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectNativeCaptureDeviceAsync(
            string deviceId, CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task AddBrowserSourceAsync(string url, int width, int height, int fps,
            CancellationToken cancellationToken = default) => throw new NotSupportedException();

        public Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default) =>
            throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
