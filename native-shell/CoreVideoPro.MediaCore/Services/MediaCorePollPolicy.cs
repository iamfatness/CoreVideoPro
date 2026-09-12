namespace CoreVideoPro.MediaCore.Services;

/// <summary>What one tick of <see cref="MediaCoreBridgeService"/>'s 250 ms poll asks for.</summary>
public readonly record struct MediaCorePollPlan(bool PollCoreSnapshot, bool RefreshZoomRoster);

/// <summary>
/// Decides what the bridge's periodic poll requests (T1.5, #432).
///
/// The core snapshot is polled on EVERY tick. The poll used to switch branch while Engine
/// (Zoom capture) was off in a meeting: it asked only for the Zoom roster, and
/// <see cref="ZoomCaptureSnapshotMerger"/> carried the old core fields forward. So
/// <c>/snapshot</c> aged, <c>RawReceivedUtc</c> and the program frame count froze, and meters
/// and output health bound to the last snapshot went quietly stale. That lasted until Engine
/// On, 67 minutes in one live session. An operator reading the frozen state saw it as a core
/// wedge.
///
/// Polling the core with Engine off costs the same as it already does every 250 ms with Engine
/// on. An empty <c>media-core-sync</c> runs no tick (CLAUDE.md, Phase 2 audio notes), but it is not
/// free: the core takes <c>coreMutex</c> and builds <c>sessionState()</c>, and the shell's single
/// sync slot is held for the round trip. A single-send sync that collides with the poll is
/// therefore skipped for backpressure, and must re-arm itself (<c>SingleSendBackpressure</c>). The
/// roster refresh is still needed while capture is off, because nothing else brings the roster:
/// the spine sync, which carries it while Engine is on, is not running.
/// </summary>
public static class MediaCorePollPolicy
{
    public static MediaCorePollPlan Plan(bool zoomSpineSyncConfigured, string? meetingState) =>
        new(
            PollCoreSnapshot: true,
            RefreshZoomRoster: !zoomSpineSyncConfigured &&
                ZoomMediaSpineSnapshotMerger.NormalizeMeetingState(meetingState)
                    .Equals("in_meeting", StringComparison.Ordinal));
}
