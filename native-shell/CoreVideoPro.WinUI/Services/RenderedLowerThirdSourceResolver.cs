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
        var eligible = frame.VideoSources.Where(source =>
        {
            if (source.Kind is not ("participant-video" or "screen-share")) return false;
            if (source.SourceId.StartsWith("capture:", StringComparison.Ordinal))
                return captureDevices.Any(device => "capture:" + device.Id == source.SourceId);
            return participants.Any(participant => participant.Id == source.ParticipantId &&
                participant.Health != FeedHealth.VideoOff && source.SourceId == "zoom:" + participant.Id);
        }).ToList();
        return eligible.FirstOrDefault(source => source.SourceId == stickySourceId)
            ?? eligible.FirstOrDefault(source => source.Kind == "participant-video")
            ?? eligible.FirstOrDefault();
    }
}
