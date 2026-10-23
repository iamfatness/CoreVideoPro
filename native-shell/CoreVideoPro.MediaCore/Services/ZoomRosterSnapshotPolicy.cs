using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Admission rule for complete Zoom roster snapshots. The core owns the roster;
/// the shell only installs a newer owner snapshot into its read model.
/// </summary>
public static class ZoomRosterSnapshotPolicy
{
    /// <summary>
    /// #766: does this snapshot carry a roster fact at all? The engine publishes
    /// <c>rosterRevision</c> 0 until its first versioned roster for an epoch (and a sync
    /// snapshot from an idle core has no epoch). Such a snapshot states nothing about the
    /// roster: it must not install a barrier, must not be counted as a stale snapshot, and the
    /// installed roster is carried. Only revision 1+ with an epoch is an observation.
    /// </summary>
    public static bool HasRosterFact(string? epoch, long revision) =>
        !string.IsNullOrEmpty(epoch) && revision > 0;

    public static bool IsNewerFact(RawCaptureSnapshot current, RawCaptureSnapshot candidate)
    {
        if (current.RosterEpoch == candidate.RosterEpoch)
            return candidate.RosterRevision > current.RosterRevision;
        return TryOrder(candidate.RosterEpoch ?? "", out var next) &&
               TryOrder(current.RosterEpoch ?? "", out var prior) && next.CompareTo(prior) > 0;
    }

    /// <summary>Direct facts are edge observations: duplicates cannot rewrite an installed barrier.</summary>
    public static bool AcceptFact(NativeMediaCoreStateSnapshot? current, string? epoch, long revision) =>
        Accept(current, epoch, revision) &&
        (current is null || current.RosterEpoch != epoch || revision > current.RosterRevision);

    public static bool Accept(NativeMediaCoreStateSnapshot? current, string? epoch, long revision)
    {
        if (current is null || string.IsNullOrEmpty(current.RosterEpoch) || current.RosterRevision <= 0)
            return true;
        if (string.IsNullOrEmpty(epoch) || revision <= 0)
            return false;
        if (current.RosterEpoch == epoch)
            return revision >= current.RosterRevision;

        // Core process generation and meeting generation are both monotonic
        // within this bridge session. The trailing engine token is diagnostic.
        // A core restart clears the bridge snapshot before accepting new data.
        return TryOrder(epoch, out var candidate) &&
               TryOrder(current.RosterEpoch, out var installed) &&
               candidate.CompareTo(installed) > 0;
    }

    private static bool TryOrder(string epoch, out (long Process, long Meeting) order)
    {
        order = default;
        var first = epoch.IndexOf(':');
        var second = first < 0 ? -1 : epoch.IndexOf(':', first + 1);
        if (first <= 0 || second <= first + 1 ||
            !long.TryParse(epoch.AsSpan(0, first), out var process) ||
            !long.TryParse(epoch.AsSpan(first + 1, second - first - 1), out var meeting))
            return false;
        order = (process, meeting);
        return true;
    }
}
