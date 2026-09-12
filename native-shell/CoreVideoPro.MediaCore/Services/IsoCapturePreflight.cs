namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// #470 / T3.7. ISO streams armed while Zoom capture is OFF write nothing, and
/// said nothing.
///
/// Seen live 2026-09-10: recording started with ISOs enabled and 7 Zoom guests
/// selected while Engine was off. Program recorded normally; all 7 ISO streams
/// sat at framesWritten 0 with no file, recording.warning null and no per-stream
/// warning. Turning Engine on mid-recording opened the writers lazily at the
/// first frame, so the operator silently lost the head of every ISO.
///
/// It cannot be caught downstream: an ISO writer opens lazily at its first
/// frame, so a source that never produces one has no stream to carry a warning.
/// The only place that knows is the arming decision itself.
///
/// Breaks ISO-1's "Loud, never silent" (a video-only-broken ISO must be as loud
/// as #286 made a video-only program).
/// </summary>
public static class IsoCapturePreflight
{
    /// <param name="isoEnabled">The "Program + ISOs" switch.</param>
    /// <param name="zoomIsoSourceCount">Selected ISO sources that come from Zoom.</param>
    /// <param name="rawMediaActive">The ENGINE-reported capture state (null when
    /// no snapshot has arrived yet), not the button we last pressed.</param>
    /// <returns>An operator-facing warning, or null when there is nothing to say.</returns>
    public static string? Describe(bool isoEnabled, int zoomIsoSourceCount, bool? rawMediaActive)
    {
        if (!isoEnabled || zoomIsoSourceCount <= 0)
        {
            return null;
        }

        // UNKNOWN is not a problem. Before the first snapshot arrives we cannot
        // tell capture-off from not-yet-reported, and warning on a state we have
        // not observed is how an indicator gets ignored.
        if (rawMediaActive is not false)
        {
            return null;
        }

        var sources = zoomIsoSourceCount == 1 ? "1 Zoom ISO source" : $"{zoomIsoSourceCount} Zoom ISO sources";
        return $"{sources} will record NOTHING while Zoom capture is off. " +
               "Turn Engine on before recording — an ISO writer opens at its first frame, " +
               "so turning it on later loses everything before that point.";
    }
}
