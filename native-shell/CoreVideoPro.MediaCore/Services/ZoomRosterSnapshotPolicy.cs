using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Admission rule for complete Zoom roster snapshots. The core owns the roster;
/// the shell only installs a newer owner snapshot into its read model.
/// </summary>
public static class ZoomRosterSnapshotPolicy
{
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
