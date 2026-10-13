using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class CoreSnapshotRevisionPolicy
{
    // Off-lock assembly can finish after a newer command response. A restarted
    // core has a new epoch and may legitimately begin again at revision zero.
    public static bool Accept(NativeMediaCoreStateSnapshot? prior, NativeMediaCoreStateSnapshot incoming)
    {
        var epoch = incoming.AudioObservation?.Epoch;
        return prior is null || string.IsNullOrEmpty(epoch) ||
            !StringComparer.Ordinal.Equals(epoch, prior.AudioObservation?.Epoch) ||
            incoming.AppliedControlRevision >= prior.AppliedControlRevision;
    }
}
