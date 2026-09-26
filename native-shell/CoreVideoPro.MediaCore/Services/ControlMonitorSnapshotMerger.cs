using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Prevents an old response from rolling applied monitor state back.</summary>
public static class ControlMonitorSnapshotMerger
{
    public static NativeMediaCoreStateSnapshot CarryNewer(
        NativeMediaCoreStateSnapshot? existing, NativeMediaCoreStateSnapshot incoming)
    {
        var previous = existing?.AudioMixSession.MonitorControl;
        var next = incoming.AudioMixSession.MonitorControl;
        if (previous is null || next is null ||
            previous.AuthorityEpoch != next.AuthorityEpoch ||
            next.Revision >= previous.Revision) return incoming;
        return incoming with { AudioMixSession = existing!.AudioMixSession };
    }
}
