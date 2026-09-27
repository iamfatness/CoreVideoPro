using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Keep the most recently applied audio route barrier on a late poll.</summary>
public static class ControlAudioRouteSnapshotMerger
{
    public static NativeMediaCoreStateSnapshot CarryNewer(
        NativeMediaCoreStateSnapshot? existing, NativeMediaCoreStateSnapshot incoming)
    {
        var previous = existing?.AudioRoutingMatrix.Control;
        var next = incoming.AudioRoutingMatrix.Control;
        if (previous is null || next is null ||
            previous.AuthorityEpoch != next.AuthorityEpoch || next.Revision >= previous.Revision)
            return incoming;
        return incoming with { AudioRoutingMatrix = existing!.AudioRoutingMatrix };
    }
}
