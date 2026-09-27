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

    [Fact]
    public void DirectRosterGapWaitsForFullBarrierAndRejectsDelayedDuplicate()
    {
        var tracker = new ControlSnapshotRecoveryTracker();
        var scheduler = new MediaCoreSyncScheduler();
        tracker.ObserveRoster("1:1:engine", 1, accepted: true);
        Assert.Equal(RosterFactAdmission.Recover,
            tracker.ObserveRosterFact("1:1:engine", 4, accepted: true));
        Assert.True(tracker.Snapshot(scheduler).RosterReconciling);
        Assert.Equal(RosterFactAdmission.Recover,
            tracker.ObserveRosterFact("1:1:engine", 5, accepted: true));
        tracker.ObserveRoster("1:1:engine", 4, accepted: true);
        Assert.True(tracker.Snapshot(scheduler).RosterReconciling);
        tracker.ObserveRoster("1:1:engine", 5, accepted: true);
        Assert.False(tracker.Snapshot(scheduler).RosterReconciling);
        Assert.Equal(RosterFactAdmission.Ignore,
            tracker.ObserveRosterFact("1:1:engine", 4, accepted: false));
        Assert.Equal(RosterFactAdmission.Apply,
            tracker.ObserveRosterFact("1:1:engine", 6, accepted: true));
        var state = tracker.Snapshot(scheduler);
        Assert.Equal(2, state.RosterMissingRevisions);
        Assert.Equal(1, state.RosterStaleSnapshots);
        Assert.Equal(2, state.SnapshotBarriers);
    }

    [Fact]
    public void RouteAndMonitorBarriersReportGapsAndRetireOnProcessReset()
    {
        var tracker = new ControlSnapshotRecoveryTracker();
        var scheduler = new MediaCoreSyncScheduler();
        tracker.ObserveAudioRoute("route-a", 0);
        tracker.ObserveAudioRoute("route-a", 3);
        tracker.ObserveAudioRoute("route-a", 1);
        tracker.ObserveMonitor("monitor-a", 0);
        tracker.ObserveMonitor("monitor-a", 2);
        var state = tracker.Snapshot(scheduler);
        Assert.Equal(2, state.AudioRouteMissingRevisions);
        Assert.Equal(1, state.AudioRouteStaleSnapshots);
        Assert.Equal(1, state.MonitorMissingRevisions);
        Assert.False(state.AudioRouteReconciling);
        tracker.ResetProcess();
        Assert.True(tracker.Snapshot(scheduler).AudioRouteReconciling);
        tracker.ObserveAudioRoute("route-b", 0);
        Assert.False(tracker.Snapshot(scheduler).AudioRouteReconciling);
    }
}
