namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>
/// When the Record control (the Studio button and the control API's <c>transport.record.set</c>)
/// may act. #1009: Program recording is a local output. It records whatever Program renders,
/// webcam and screen capture included, so it does not take the Zoom meeting as an input. Engine
/// on/off (the Zoom capture subscription) stays meeting-gated; ISO recording of Zoom guests keeps
/// its own Zoom eligibility and capture pre-flight (<c>ShowInputsCoordinator.BuildIsoSourceTargets</c>,
/// <c>IsoCapturePreflight</c>).
///
/// Whether the native core can actually record is not decided here. A start goes through
/// <see cref="TransportCoordinator.SetRecordingAsync"/>, which starts the core if it is down,
/// refuses a core whose encoder did not construct (<c>MediaCoreOnAirPolicy.RecordBlockReason</c>),
/// runs the disk pre-flight, and rolls back on a failed start, saying why each time.
/// </summary>
public static class RecordingControlPolicy
{
    /// <summary>The Studio Record button. While "Stop and close" finishes the outputs, the
    /// button is held (the close guard owns the stop).</summary>
    public static bool CanToggle(bool toggleInFlight, bool outputsClosing) =>
        !toggleInFlight && !outputsClosing;

    /// <summary>An explicit start or stop (control API, resume after a core restart). A stop is
    /// always allowed outside an in-flight toggle; a start is refused while outputs close.</summary>
    public static bool CanSet(bool requested, bool toggleInFlight, bool outputsClosing) =>
        !toggleInFlight && (!requested || !outputsClosing);
}
