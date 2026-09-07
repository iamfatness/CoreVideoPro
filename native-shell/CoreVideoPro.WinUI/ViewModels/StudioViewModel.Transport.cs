using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels.Transport;
using CommunityToolkit.Mvvm.ComponentModel;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// StudioViewModel's Transport orchestration façade (PR2 strangler). The record / stream / take /
/// engine-on-off async command BODIES live in <see cref="TransportCoordinator"/>; StudioViewModel
/// keeps the generated <c>[RelayCommand]</c> objects (ToggleEngineCommand, TakeCommand,
/// ToggleRecordingCommand, ToggleStreamingCommand) and forwards their bodies here, so XAML x:Bind
/// paths and the ~15 external <c>NotifyCanExecuteChanged</c> pokes are unchanged. StudioViewModel
/// implements <see cref="ITransportHost"/> + <see cref="ITransportDispatcher"/> over <c>this</c>
/// (the PR1 Overlays/MagicScene sub-VM pattern) — the bound transport state (Recording, Streaming,
/// OutputStatus, …) stays [ObservableProperty] on the god file; the coordinator writes it through
/// the interface. The in-flight guards moved to the coordinator; the CanExecute predicates read
/// them via <see cref="TransportCoordinator.RecordingToggleInFlight"/> /
/// <see cref="TransportCoordinator.StreamToggleInFlight"/>.
/// </summary>
public sealed partial class StudioViewModel : ITransportHost, ITransportDispatcher
{
    private readonly TransportCoordinator _transportCoordinator;

    // Intent is kept separately from Recording/Streaming, which reflect observed media.
    [ObservableProperty]
    private bool _recordingRequested;
    [ObservableProperty]
    private bool _streamingRequested;

    partial void OnRecordingRequestedChanged(bool value) => OnPropertyChanged(nameof(RecordingLabel));
    partial void OnStreamingRequestedChanged(bool value) => OnPropertyChanged(nameof(StreamingLabel));

    internal NativeMediaCoreStateSnapshot? NativeControlSnapshot => _bridge.Running ? _bridge.LastSnapshot : null;

    internal Task<TakeResult> TakeForControlAsync() => _transportCoordinator.TakeAsync();

    internal Task SetRecordingAsync(bool requested) => _transportCoordinator.SetRecordingAsync(requested);
    internal Task SetStreamingAsync(bool requested) => _transportCoordinator.SetStreamingAsync(requested);
    internal bool CanSetRecording(bool requested) => !_transportCoordinator.RecordingToggleInFlight && (!requested || Settings.IsInMeeting);
    internal bool CanSetStreaming(bool requested) => !_transportCoordinator.StreamToggleInFlight;

    // Called only on the UI thread, including the capture-independent polling path.
    private void ApplyOutputLifecyclePatch(LiveProductionSync.StudioLiveProductionPatch patch)
    {
        // Observed snapshots never overwrite operator intent. Terminal failure
        // disarms future syncs once the active command has settled.
        if (patch.RecordingRequested == false && !_transportCoordinator.RecordingToggleInFlight)
        {
            RecordingRequested = false;
        }
        if (patch.Recording is { } recording)
        {
            Recording = recording;
        }

        if (patch.Streaming is { } streaming)
        {
            Streaming = streaming;
        }

        if (patch.OutputStatus is { Length: > 0 } outputStatus)
        {
            OutputStatus = outputStatus;
        }

        if (patch.OutputSessionStatus is { Length: > 0 } outputSessionStatus)
        {
            OutputSessionStatus = outputSessionStatus;
        }
    }

    private void InterruptOutputSessions()
    {
        var interrupted = RecordingRequested || StreamingRequested || Recording || Streaming;
        RecordingRequested = false;
        StreamingRequested = false;
        Recording = false;
        Streaming = false;
        if (interrupted)
        {
            OutputStatus = "Outputs interrupted — recording continuity was lost. Start a new session after recovery.";
            OutputSessionStatus = OutputStatus;
        }
    }

    // --- ITransportDispatcher: preserve the exact RunOnUiThread marshalling semantics ---
    void ITransportDispatcher.RunOnUiThread(Action action) => RunOnUiThread(action);

    // --- ITransportHost: bound transport state (stays [ObservableProperty] on StudioViewModel) ---
    bool ITransportHost.Recording
    {
        get => RecordingRequested;
        set => RecordingRequested = value;
    }

    bool ITransportHost.Streaming
    {
        get => StreamingRequested;
        set => StreamingRequested = value;
    }

    bool ITransportHost.ZoomCaptureSubscribed
    {
        get => ZoomCaptureSubscribed;
        set => ZoomCaptureSubscribed = value;
    }

    string ITransportHost.EngineStatus
    {
        set => EngineStatus = value;
    }

    string ITransportHost.CommandStatus
    {
        set => CommandStatus = value;
    }

    string ITransportHost.OutputStatus
    {
        get => OutputStatus;
        set => OutputStatus = value;
    }

    string ITransportHost.OutputSessionStatus
    {
        set => OutputSessionStatus = value;
    }

    string? ITransportHost.RecordingDiskWarning
    {
        set => RecordingDiskWarning = value;
    }

    string? ITransportHost.SelectedParticipantId => SelectedParticipantId;

    // --- scene / take machinery ---
    string ITransportHost.ActiveSceneId
    {
        get => ActiveSceneId;
        set => ActiveSceneId = value;
    }

    string ITransportHost.PreviewSceneId
    {
        get => PreviewSceneId;
        set => PreviewSceneId = value;
    }

    string ITransportHost.ProgramSceneSummary => ProgramSceneSummary;

    string ITransportHost.TakeTransitionLabel => TakeTransitionLabel;

    bool ITransportHost.IsSceneAvailable(string? sceneId) =>
        !string.IsNullOrWhiteSpace(sceneId) && Scenes.Any(scene => scene.Id == sceneId);

    public bool HasPendingPreviewTake =>
        _livePreviewDraft is not null &&
        string.Equals(_livePreviewDraftSceneId, ActiveSceneId, StringComparison.Ordinal) &&
        SceneTakeRules.HasPendingChanges(GetMutableRoutes(ActiveSceneId), _livePreviewDraft);

    bool ITransportHost.HasPendingPreviewChanges(string sceneId) =>
        string.Equals(sceneId, _livePreviewDraftSceneId, StringComparison.Ordinal) && HasPendingPreviewTake;

    private int _takeMutationDepth;
    void ITransportHost.BeginTakeMutation() => _takeMutationDepth++;
    void ITransportHost.EndTakeMutation() => _takeMutationDepth--;
    void ITransportHost.RequestTakeReconciliation() => QueueProductionSyncRetry("take-rollback");

    // Capture originals now, then seal ownership after the local Take mutations.
    // A rollback must not erase edits made while the media-core reply was pending.
    Func<Func<bool>> ITransportHost.CaptureTakeRollback()
    {
        var program = ActiveSceneId;
        var preview = PreviewSceneId;
        var programRoutes = GetMutableRoutes(program).Select(route => route.Clone()).ToList();
        var previewRoutes = GetMutableRoutes(preview).Select(route => route.Clone()).ToList();
        var draft = _livePreviewDraft?.Select(route => route.Clone()).ToList();
        var draftScene = _livePreviewDraftSceneId;
        var playbackVersion = _programMediaPlaybackTakeVersion;
        return () =>
        {
            var attemptedProgram = ActiveSceneId;
            var attemptedPreview = PreviewSceneId;
            var expectedProgram = GetMutableRoutes(program).Select(route => route.Clone()).ToList();
            var expectedPreview = GetMutableRoutes(preview).Select(route => route.Clone()).ToList();
            var expectedDraft = _livePreviewDraft?.Select(route => route.Clone()).ToList();
            return () =>
            {
                if (!((ITransportHost)this).IsSceneAvailable(program) ||
                    !((ITransportHost)this).IsSceneAvailable(preview) ||
                    ActiveSceneId != attemptedProgram ||
                    SceneTakeRules.HasPendingChanges(expectedProgram, GetMutableRoutes(program)) ||
                    SceneTakeRules.HasPendingChanges(expectedPreview, GetMutableRoutes(preview)) ||
                    SceneTakeRules.HasPendingChanges(expectedDraft ?? [], _livePreviewDraft ?? []))
                    return false;
                _takeMutationDepth++;
                try
                {
                    var stillOwnsPreview = PreviewSceneId == attemptedPreview;
                    ActiveSceneId = program;
                    if (stillOwnsPreview) PreviewSceneId = preview;
                    GetMutableRoutes(program).Clear();
                    GetMutableRoutes(program).AddRange(programRoutes);
                    if (program != preview)
                    {
                        GetMutableRoutes(preview).Clear();
                        GetMutableRoutes(preview).AddRange(previewRoutes);
                    }
                    if (stillOwnsPreview)
                    {
                        _livePreviewDraft = draft;
                        _livePreviewDraftSceneId = draftScene;
                    }
                    _programMediaPlaybackTakeVersion = playbackVersion;
                    RefreshPreviewRoutingState();
                    OnPropertyChanged(nameof(CanTake));
                    TakeCommand.NotifyCanExecuteChanged();
                    return true;
                }
                finally { _takeMutationDepth--; }
            };
        };
    }

    void ITransportHost.CopyPreviewRoutesToScene(string sceneId) => CopyPreviewRoutesToScene(sceneId);

    void ITransportHost.PromoteProgramMediaRouteToPlayback() => PromoteProgramMediaRouteToPlayback();

    void ITransportHost.RefreshPreviewRoutingState() => RefreshPreviewRoutingState();

    void ITransportHost.IncrementProgramMediaPlaybackTakeVersion() => _programMediaPlaybackTakeVersion++;

    // --- media-core lifecycle + sync (stay on the god file; the coordinator calls through) ---
    Task ITransportHost.EnsureMediaCoreRunningAsync(string startingStatus) =>
        EnsureMediaCoreRunningAsync(startingStatus);

    Task<NativeMediaCoreStateSnapshot> ITransportHost.SyncActiveSceneAsync(string? reason) =>
        SyncActiveSceneAsync(reason);

    Task<Dictionary<string, object?>> ITransportHost.BuildSpinePayloadAsync(CancellationToken cancellationToken) => CaptureUiOwnedAsync(BuildSpinePayload, cancellationToken);

    void ITransportHost.UnsubscribeZoomCapture(string status) => UnsubscribeZoomCapture(status);

    void ITransportHost.NotifySurfacesCaptureSubscribed(bool subscribed, string? compositorRenderer) =>
        _surfaces.SetZoomCaptureSubscribed(subscribed, compositorRenderer);

    void ITransportHost.NotifySurfacesPreviewParticipant(string? participantId) =>
        _surfaces.SetPreviewParticipant(participantId);

    void ITransportHost.RefreshSdkReadiness() => Settings.RefreshSdkReadiness();

    void ITransportHost.RefreshSurfaceBindings() => RefreshSurfaceBindings();

    void ITransportHost.RefreshTransportState() => RefreshTransportState();

    void ITransportHost.RefreshOutputStatus() => RefreshOutputStatus();

    // --- recording pre-flight + stream destinations ---
    bool ITransportHost.TryEvaluateRecordingDiskPreflight(out IsoDiskPreflightResult result) =>
        TryEvaluateRecordingDiskPreflight(out result);

    IReadOnlyList<string> ITransportHost.BuildSelectedStreamDestinations(bool validatedOnly) =>
        BuildSelectedStreamDestinations(validatedOnly);

    string? ITransportHost.ValidateStreamDestinations() => ValidateStreamDestinations();

    // --- command CanExecute pokes (commands stay generated on StudioViewModel) ---
    void ITransportHost.NotifyRecordingCommandCanExecuteChanged() =>
        ToggleRecordingCommand.NotifyCanExecuteChanged();

    void ITransportHost.NotifyStreamingCommandCanExecuteChanged() =>
        ToggleStreamingCommand.NotifyCanExecuteChanged();

    // --- log-line data (kept byte-identical to the original bodies) ---
    string ITransportHost.RecordingLogFormat => NormalizeRecordingFormat(RecordingFormat);

    double ITransportHost.RecordingLogBitrateMbps => NormalizeOutputTargetBitrateMbps(RecordingTargetBitrateMbps);

    string ITransportHost.RecordingVideoCodec => RecordingVideoCodec;

    bool ITransportHost.StreamRtmpEnabled => StreamRtmpEnabled;

    bool ITransportHost.StreamNdiEnabled => StreamNdiEnabled;

    bool ITransportHost.StreamSrtEnabled => StreamSrtEnabled;

    double ITransportHost.StreamLogBitrateMbps => NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps);

    string ITransportHost.StreamVideoCodec => StreamVideoCodec;

    string ITransportHost.StreamEncoderMode => StreamEncoderMode;

    string ITransportHost.FormatStreamDestinationTelemetry(IReadOnlyList<string> destinations) =>
        FormatStreamDestinationTelemetry(destinations);
}
