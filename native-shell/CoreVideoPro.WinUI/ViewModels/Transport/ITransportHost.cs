using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>
/// A minimal UI-thread marshalling seam so <see cref="TransportCoordinator"/> can preserve the
/// exact <c>RunOnUiThread</c> semantics of the original StudioViewModel command bodies (the
/// transport commands <c>ConfigureAwait(false)</c> then marshal every bound-property write back
/// to the UI thread — the 0xc000027b rule). The production implementation forwards to
/// StudioViewModel's private <c>RunOnUiThread</c> (DispatcherQueue); tests supply an inline one.
/// </summary>
public interface ITransportDispatcher
{
    void RunOnUiThread(Action action);
}

/// <summary>
/// The cross-cluster surface <see cref="TransportCoordinator"/> needs from the owning
/// <c>StudioViewModel</c> — the bound transport state it writes, the scene-draft/Take machinery,
/// the media-core lifecycle + sync entry points, the readout refreshes, and the command
/// <c>NotifyCanExecuteChanged</c> pokes. StudioViewModel implements this over <c>this</c> (the
/// PR1 Overlays/MagicScene sub-VM pattern). Keeping it an interface is what makes the coordinator
/// independently constructible for the characterization tests the god object never allowed.
///
/// Move-only: every member mirrors an existing StudioViewModel property/method; the coordinator
/// just routes the original bodies through it instead of <c>this</c>.
/// </summary>
public interface ITransportHost
{
    // --- transport bound state (the command bodies read/write these; they stay
    //     [ObservableProperty] on StudioViewModel so XAML x:Bind is unchanged) ---
    bool Recording { get; set; }

    bool Streaming { get; set; }

    bool ZoomCaptureSubscribed { get; set; }

    string EngineStatus { set; }

    string CommandStatus { set; }

    string OutputStatus { get; set; }

    string OutputSessionStatus { set; }

    string? RecordingDiskWarning { set; }

    string? SelectedParticipantId { get; }

    // --- scene / take machinery ---
    string ActiveSceneId { get; set; }

    string PreviewSceneId { get; set; }

    string ProgramSceneSummary { get; }

    string TakeTransitionLabel { get; }

    /// <summary>Wraps the original <c>_livePreviewDraft is not null &amp;&amp;
    /// HasPendingProgramMediaCue(GetMutableRoutes(sceneId), _livePreviewDraft)</c> guard so the
    /// draft stays private to StudioViewModel.</summary>
    bool HasPendingProgramMediaCue(string sceneId);

    void CopyPreviewRoutesToScene(string sceneId);

    void PromoteProgramMediaRouteToPlayback();

    void RefreshPreviewRoutingState();

    void IncrementProgramMediaPlaybackTakeVersion();

    // --- media-core lifecycle + sync (stay on the god file; the coordinator calls through) ---
    Task EnsureMediaCoreRunningAsync(string startingStatus);

    Task<NativeMediaCoreStateSnapshot> SyncActiveSceneAsync(string? reason = null);

    Dictionary<string, object?> BuildSpinePayload();

    void UnsubscribeZoomCapture(string status);

    void NotifySurfacesCaptureSubscribed(bool subscribed, string? compositorRenderer);

    void NotifySurfacesPreviewParticipant(string? participantId);

    void RefreshSdkReadiness();

    void RefreshSurfaceBindings();

    void RefreshTransportState();

    void RefreshOutputStatus();

    // --- recording pre-flight + stream destinations ---
    bool TryEvaluateRecordingDiskPreflight(out IsoDiskPreflightResult result);

    IReadOnlyList<string> BuildSelectedStreamDestinations(bool validatedOnly);

    string? ValidateStreamDestinations();

    // --- command CanExecute pokes (commands stay generated on StudioViewModel) ---
    void NotifyRecordingCommandCanExecuteChanged();

    void NotifyStreamingCommandCanExecuteChanged();

    // --- log-line data (kept identical to the original bodies) ---
    string RecordingLogFormat { get; }

    double RecordingLogBitrateMbps { get; }

    string RecordingVideoCodec { get; }

    bool StreamRtmpEnabled { get; }

    bool StreamNdiEnabled { get; }

    bool StreamSrtEnabled { get; }

    double StreamLogBitrateMbps { get; }

    string StreamVideoCodec { get; }

    string StreamEncoderMode { get; }

    string FormatStreamDestinationTelemetry(IReadOnlyList<string> destinations);
}
