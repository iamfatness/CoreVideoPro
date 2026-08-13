using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.ViewModels.Transport;

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

    // --- ITransportDispatcher: preserve the exact RunOnUiThread marshalling semantics ---
    void ITransportDispatcher.RunOnUiThread(Action action) => RunOnUiThread(action);

    // --- ITransportHost: bound transport state (stays [ObservableProperty] on StudioViewModel) ---
    bool ITransportHost.Recording
    {
        get => Recording;
        set => Recording = value;
    }

    bool ITransportHost.Streaming
    {
        get => Streaming;
        set => Streaming = value;
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

    bool ITransportHost.HasPendingProgramMediaCue(string sceneId) =>
        _livePreviewDraft is not null &&
        HasPendingProgramMediaCue(GetMutableRoutes(sceneId), _livePreviewDraft);

    void ITransportHost.CopyPreviewRoutesToScene(string sceneId) => CopyPreviewRoutesToScene(sceneId);

    void ITransportHost.PromoteProgramMediaRouteToPlayback() => PromoteProgramMediaRouteToPlayback();

    void ITransportHost.RefreshPreviewRoutingState() => RefreshPreviewRoutingState();

    void ITransportHost.IncrementProgramMediaPlaybackTakeVersion() => _programMediaPlaybackTakeVersion++;

    // --- media-core lifecycle + sync (stay on the god file; the coordinator calls through) ---
    Task ITransportHost.EnsureMediaCoreRunningAsync(string startingStatus) =>
        EnsureMediaCoreRunningAsync(startingStatus);

    Task<NativeMediaCoreStateSnapshot> ITransportHost.SyncActiveSceneAsync(string? reason) =>
        SyncActiveSceneAsync(reason);

    Dictionary<string, object?> ITransportHost.BuildSpinePayload() => BuildSpinePayload();

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
