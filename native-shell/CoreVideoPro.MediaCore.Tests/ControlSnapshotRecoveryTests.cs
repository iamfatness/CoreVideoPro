using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ControlSnapshotRecoveryTests
{
    [Fact]
    public void CompleteSnapshotAfterGapIsBarrierAndOldResponseIsRejected()
    {
        var tracker = new ControlSnapshotRecoveryTracker();
        var scheduler = new MediaCoreSyncScheduler();
        tracker.ObserveRoster("1:1:a", 1, accepted: true);
        tracker.ObserveRoster("1:1:a", 4, accepted: true);
        tracker.ObserveRoster("1:1:a", 2, accepted: false);
        tracker.ObserveMonitor("core-a", 1);
        tracker.ObserveMonitor("core-a", 3);
        tracker.ObserveMonitor("core-a", 2);
        var state = tracker.Snapshot(scheduler);
        Assert.Equal(2, state.RosterMissingRevisions);
        Assert.Equal(1, state.RosterStaleSnapshots);
        Assert.Equal(1, state.MonitorMissingRevisions);
        Assert.Equal(1, state.MonitorStaleSnapshots);
        Assert.Equal(4, state.SnapshotBarriers);

        tracker.ResetProcess();
        tracker.ObserveRoster("1:1:old", 1, accepted: true);
        Assert.Equal(1, tracker.Snapshot(scheduler).ProcessResets);
    }

    [Fact]
    public void DelayedOldCoreResponseCannotRollbackAppliedMonitor()
    {
        static NativeMediaCoreStateSnapshot Snapshot(long revision, bool enabled) => new()
        {
            AudioMixSession = new NativeMediaCoreAudioMixSession
            {
                Status = "armed", Summary = "Monitor", MonitorEnabled = enabled,
                MonitorControl = new NativeMediaCoreMonitorControl
                {
                    AuthorityEpoch = "core-a", Revision = revision
                }
            }
        };
        var current = Snapshot(3, true);
        var delayed = Snapshot(1, false);
        var merged = ControlMonitorSnapshotMerger.CarryNewer(current, delayed);
        Assert.True(merged.AudioMixSession.MonitorEnabled);
        Assert.Equal(3, merged.AudioMixSession.MonitorControl!.Revision);
    }
}
