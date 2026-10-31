using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class RenderedLowerThirdSourceResolver
{
    public static NativeMediaCoreRenderedVideoSource? Resolve(string activeSceneId,
        NativeMediaCoreProgramFrame? frame, string stickySourceId,
        IReadOnlyList<Participant> participants, IReadOnlyList<CaptureDevice> captureDevices)
    {
        if (frame is null || frame.FrameNumber <= 0 || frame.SceneId != activeSceneId || frame.VideoSources is null)
            return null;
        // Transition attribution legitimately contains both pictures. Sticky
        // selection must not keep the outgoing subject after this Take ends.
        // These are native rendered layer selectors, not guessed route intent.
        var incomingTake = frame.RenderPlanId?.StartsWith("take:", StringComparison.Ordinal) == true;
        var eligible = frame.VideoSources.Where(source =>
        {
            if (incomingTake && source.LayerId?.StartsWith("take-in:", StringComparison.Ordinal) != true) return false;
            // A media clip routed into a scene is a lower-third subject too (owner, 2026-10-31:
            // the F1 / "LT in" subject must resolve on a media-only test show). Backgrounds are not.
            if (source.Kind == "media-video" && source.SourceId.StartsWith("media:", StringComparison.Ordinal)) return true;
            if (source.Kind is not ("participant-video" or "screen-share")) return false;
            if (source.SourceId.StartsWith("capture:", StringComparison.Ordinal))
                return captureDevices.Any(device => "capture:" + device.Id == source.SourceId);
            return participants.Any(participant => participant.Id == source.ParticipantId &&
                participant.Health != FeedHealth.VideoOff && source.SourceId == "zoom:" + participant.Id);
        }).ToList();
        return eligible.FirstOrDefault(source => source.SourceId == stickySourceId)
            ?? eligible.FirstOrDefault(source => source.Kind == "participant-video")
            ?? eligible.FirstOrDefault(source => source.Kind == "media-video")
            ?? eligible.FirstOrDefault();
    }
}
