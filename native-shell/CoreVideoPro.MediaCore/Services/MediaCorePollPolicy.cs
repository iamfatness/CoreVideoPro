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
/// Polling the core is cheap and has no side effects. An empty <c>media-core-sync</c> returns
/// the core's already-published snapshot without running a tick (CLAUDE.md, Phase 2 audio
/// notes). The roster refresh is still needed while capture is off, because nothing else
/// brings the roster: the spine sync, which carries it while Engine is on, is not running.
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
