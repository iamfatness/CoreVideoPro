namespace CoreVideoPro.MediaCore.Services;

public sealed record ControlSnapshotRecoveryTelemetry(
    long RosterMissingRevisions, long RosterStaleSnapshots,
    long MonitorMissingRevisions, long MonitorStaleSnapshots,
    long SnapshotBarriers, long ProcessResets,
    long CommandOverloads, long CoalescedPolls, int WaitingCommands);

/// <summary>
/// Tracks recovery evidence for complete, versioned snapshots. A newer full
/// snapshot is itself a barrier after a missing revision; no callback schedules
/// another sync and no high-rate media event is placed on the control queue.
/// </summary>
internal sealed class ControlSnapshotRecoveryTracker
{
    private readonly Domain _roster = new();
    private readonly Domain _monitor = new();
    private long _processResets;

    public void ResetProcess()
    {
        _roster.Reset();
        _monitor.Reset();
        _processResets++;
    }

    public void ObserveRoster(string? epoch, long revision, bool accepted) =>
        _roster.Observe(epoch, revision, accepted);

    public void ObserveMonitor(string? epoch, long revision) =>
        _monitor.Observe(epoch, revision, accepted: true);

    public ControlSnapshotRecoveryTelemetry Snapshot(MediaCoreSyncScheduler scheduler) => new(
        _roster.MissingRevisions, _roster.StaleSnapshots,
        _monitor.MissingRevisions, _monitor.StaleSnapshots,
        _roster.Barriers + _monitor.Barriers, _processResets,
        scheduler.OverloadCount, scheduler.CoalescedPollCount,
        scheduler.WaitingCommands);

    private sealed class Domain
    {
        private string _epoch = string.Empty;
        private long _revision;
        public long MissingRevisions { get; private set; }
        public long StaleSnapshots { get; private set; }
        public long Barriers { get; private set; }

        public void Reset()
        {
            _epoch = string.Empty;
            _revision = 0;
        }

        public void Observe(string? epoch, long revision, bool accepted)
        {
            if (!accepted)
            {
                StaleSnapshots++;
                return;
            }
            if (string.IsNullOrWhiteSpace(epoch) || revision < 0) return;
            if (_epoch != epoch)
            {
                _epoch = epoch;
                _revision = revision;
                Barriers++;
                return;
            }
            if (revision < _revision)
            {
                StaleSnapshots++;
                return;
            }
            if (revision > _revision + 1)
            {
                MissingRevisions += revision - _revision - 1;
                Barriers++;
            }
            _revision = revision;
        }
    }
}
