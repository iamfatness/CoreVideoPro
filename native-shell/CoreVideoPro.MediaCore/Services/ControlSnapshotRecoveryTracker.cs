namespace CoreVideoPro.MediaCore.Services;

public sealed record ControlSnapshotRecoveryTelemetry(
    long RosterMissingRevisions, long RosterStaleSnapshots,
    long MonitorMissingRevisions, long MonitorStaleSnapshots,
    long AudioRouteMissingRevisions, long AudioRouteStaleSnapshots,
    long SnapshotBarriers, long ProcessResets,
    long CommandOverloads, long CoalescedPolls, int WaitingCommands,
    bool RosterReconciling, bool MonitorReconciling, bool AudioRouteReconciling,
    long RosterCoalescedFacts, long RosterDiscardedOlderFacts);

internal enum RosterFactAdmission { Apply, Ignore, Recover }

/// <summary>
/// Tracks recovery evidence for complete, versioned snapshots. A newer full
/// snapshot is itself a barrier after a missing revision; no callback schedules
/// another sync and no high-rate media event is placed on the control queue.
/// </summary>
internal sealed class ControlSnapshotRecoveryTracker
{
    private readonly Domain _roster = new();
    private readonly Domain _monitor = new();
    private readonly Domain _audioRoute = new();
    private long _processResets;
    public bool RosterReconciling => _roster.Reconciling;

    public void ResetProcess()
    {
        _roster.Reset();
        _monitor.Reset();
        _audioRoute.Reset();
        _processResets++;
    }

    public void ObserveRoster(string? epoch, long revision, bool accepted) =>
        _roster.ObserveBarrier(epoch, revision, accepted);

    public RosterFactAdmission ObserveRosterFact(string? epoch, long revision, bool accepted) =>
        _roster.ObserveFact(epoch, revision, accepted);

    public void ObserveMonitor(string? epoch, long revision) =>
        _monitor.ObserveBarrier(epoch, revision, accepted: true);

    public void ObserveAudioRoute(string? epoch, long revision) =>
        _audioRoute.ObserveBarrier(epoch, revision, accepted: true);

    public ControlSnapshotRecoveryTelemetry Snapshot(MediaCoreSyncScheduler scheduler,
        long rosterCoalescedFacts = 0, long rosterDiscardedOlderFacts = 0) => new(
        _roster.MissingRevisions, _roster.StaleSnapshots,
        _monitor.MissingRevisions, _monitor.StaleSnapshots,
        _audioRoute.MissingRevisions, _audioRoute.StaleSnapshots,
        _roster.Barriers + _monitor.Barriers + _audioRoute.Barriers, _processResets,
        scheduler.OverloadCount, scheduler.CoalescedPollCount,
        scheduler.WaitingCommands,
        _roster.Reconciling, _monitor.Reconciling, _audioRoute.Reconciling,
        rosterCoalescedFacts, rosterDiscardedOlderFacts);

    private sealed class Domain
    {
        private string _epoch = string.Empty;
        private long _revision;
        private string _pendingEpoch = string.Empty;
        private long _pendingRevision;
        public long MissingRevisions { get; private set; }
        public long StaleSnapshots { get; private set; }
        public long Barriers { get; private set; }
        public bool Reconciling { get; private set; } = true;

        public void Reset()
        {
            _epoch = string.Empty;
            _revision = 0;
            _pendingEpoch = string.Empty;
            _pendingRevision = 0;
            Reconciling = true;
        }

        public RosterFactAdmission ObserveFact(string? epoch, long revision, bool accepted)
        {
            if (!accepted || string.IsNullOrWhiteSpace(epoch) || revision <= 0 ||
                (_epoch == epoch && revision <= _revision))
            {
                StaleSnapshots++;
                return RosterFactAdmission.Ignore;
            }
            if (Reconciling)
            {
                if (_pendingEpoch != epoch)
                {
                    _pendingEpoch = epoch;
                    _pendingRevision = revision;
                }
                else if (revision > _pendingRevision)
                    _pendingRevision = revision;
                return RosterFactAdmission.Recover;
            }
            if (_epoch != epoch || _revision == 0 || revision > _revision + 1)
            {
                if (_epoch == epoch && _revision > 0)
                    MissingRevisions += revision - _revision - 1;
                _pendingEpoch = epoch;
                _pendingRevision = revision;
                Reconciling = true;
                return RosterFactAdmission.Recover;
            }
            _revision = revision;
            return RosterFactAdmission.Apply;
        }

        public void ObserveBarrier(string? epoch, long revision, bool accepted)
        {
            if (!accepted)
            {
                StaleSnapshots++;
                return;
            }
            if (string.IsNullOrWhiteSpace(epoch) || revision < 0) return;
            if (Reconciling && _pendingEpoch == epoch && revision < _pendingRevision) return;
            var repairedFactGap = Reconciling && _pendingEpoch == epoch;
            if (_epoch != epoch)
            {
                _epoch = epoch;
                _revision = revision;
                Barriers++;
                Reconciling = false;
                return;
            }
            if (revision < _revision)
            {
                StaleSnapshots++;
                return;
            }
            if (revision > _revision + 1)
            {
                if (!repairedFactGap) MissingRevisions += revision - _revision - 1;
                Barriers++;
            }
            _revision = revision;
            Reconciling = false;
            _pendingEpoch = string.Empty;
            _pendingRevision = 0;
        }
    }
}
