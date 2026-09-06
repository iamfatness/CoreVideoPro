using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;
namespace CoreVideoPro.WinUI.Tests;

public sealed class RenderedLowerThirdSourceResolverTests
{
    private static readonly Participant[] Participants = [
        new() { Id = "jamal", Name = "Jamal", Health = FeedHealth.Live },
        new() { Id = "anika", Name = "Anika", Health = FeedHealth.Live }];
    private static NativeMediaCoreRenderedVideoSource Source(string id, string kind = "participant-video") =>
        new() { LayerId = "layer-" + id, SourceId = "zoom:" + id, ParticipantId = id, Kind = kind };
    private static NativeMediaCoreProgramFrame Frame(params NativeMediaCoreRenderedVideoSource[] sources) =>
        new() { FrameNumber = 10, SceneId = "interview", RenderPlanId = "interview:2:0", Health = "live", VideoSources = sources };

    [Fact]
    public void SilentRoomUsesActualRenderedFallbackAndKeepsStickyOnlyWhilePresent()
    {
        Assert.All(Participants, participant => Assert.False(participant.IsActiveSpeaker));
        var frame = Frame(Source("jamal"), Source("anika"));
        Assert.Equal("zoom:jamal", RenderedLowerThirdSourceResolver.Resolve("interview", frame, "", Participants, [])?.SourceId);
        Assert.Equal("zoom:anika", RenderedLowerThirdSourceResolver.Resolve("interview", frame, "zoom:anika", Participants, [])?.SourceId);
        Assert.Equal("zoom:jamal", RenderedLowerThirdSourceResolver.Resolve("interview", Frame(Source("jamal")), "zoom:anika", Participants, [])?.SourceId);
    }

    [Fact]
    public void NoFallbackToOtherSceneMissingMetadataOrArbitraryRoster()
    {
        Assert.Null(RenderedLowerThirdSourceResolver.Resolve("panel", Frame(Source("jamal")), "", Participants, []));
        Assert.Null(RenderedLowerThirdSourceResolver.Resolve("interview", Frame(Source("missing-guest")), "", Participants, []));
        Assert.Null(RenderedLowerThirdSourceResolver.Resolve("interview", Frame(), "", Participants, []));
        Assert.Null(RenderedLowerThirdSourceResolver.Resolve("interview", Frame() with { VideoSources = null }, "", Participants, []));
        Assert.Null(RenderedLowerThirdSourceResolver.Resolve("interview", Frame(Source("jamal", "media-video")), "", Participants, []));
    }

    [Fact]
    public void CaptureAndScreenShareRequireExactCurrentSourceMetadata()
    {
        var camera = new CaptureDevice
        {
            Id = "camera", NativeDeviceId = "camera", Name = "Presenter camera",
            Vendor = "UVC", Inputs = [], SelectedInputId = ""
        };
        var capture = new NativeMediaCoreRenderedVideoSource
        {
            SourceId = "capture:camera", ParticipantId = "capture:camera", Kind = "participant-video"
        };
        Assert.Equal(capture, RenderedLowerThirdSourceResolver.Resolve("interview", Frame(capture), "", Participants, [camera]));
        Assert.Null(RenderedLowerThirdSourceResolver.Resolve("interview", Frame(capture), "", Participants, []));
        var share = Source("anika", "screen-share");
        Assert.Equal(share, RenderedLowerThirdSourceResolver.Resolve("interview", Frame(share), "", Participants, []));
        Assert.Equal("zoom:jamal", RenderedLowerThirdSourceResolver.Resolve("interview", Frame(share, Source("jamal")), "", Participants, [])?.SourceId);
    }

    [Fact]
    public void SameSceneEditWaitsForCurrentAckAndThenANewerRenderedFrame()
    {
        var freshness = new RenderedProgramFreshness();
        freshness.Observe("interview:jamal");
        var previous = freshness.Revision;
        freshness.Acknowledge(previous, 8);
        Assert.True(freshness.Accepts(Frame(Source("jamal"))));
        freshness.Observe("interview:anika");
        Assert.False(freshness.Accepts(Frame(Source("jamal"))));
        freshness.Acknowledge(previous, 9); // stale request must not clear the gate
        Assert.False(freshness.Accepts(Frame(Source("jamal"))));
        freshness.Acknowledge(freshness.Revision, 10);
        Assert.False(freshness.Accepts(Frame(Source("anika"))));
        Assert.True(freshness.Accepts(Frame(Source("anika")) with { FrameNumber = 11 }));
        freshness.Acknowledge(freshness.Revision, 20); // ordinary polls do not re-arm
        Assert.True(freshness.Accepts(Frame(Source("anika")) with { FrameNumber = 11 }));
    }
    [Fact]
    public void SourceChangeAndTakeCannotReuseOldNameBeforeNewRenderedEvidence()
    {
        var freshness = new RenderedProgramFreshness();
        freshness.Observe("interview:jamal");
        freshness.Acknowledge(freshness.Revision, 8);
        var oldFrame = Frame(Source("jamal"));
        string? Resolve(string scene, NativeMediaCoreProgramFrame frame) => freshness.Accepts(frame)
            ? RenderedLowerThirdSourceResolver.Resolve(scene, frame, "zoom:jamal", Participants, [])?.SourceId
            : null;
        Assert.Equal("zoom:jamal", Resolve("interview", oldFrame));
        freshness.Observe("interview:anika");
        Assert.Null(Resolve("interview", oldFrame));
        freshness.Acknowledge(freshness.Revision, oldFrame.FrameNumber);
        Assert.Null(Resolve("interview", oldFrame));
        var nextFrame = Frame(Source("anika")) with { FrameNumber = 11 };
        Assert.Equal("zoom:anika", Resolve("interview", nextFrame));
        freshness.Observe("panel:anika");
        freshness.Acknowledge(freshness.Revision, 11);
        Assert.Null(Resolve("panel", nextFrame with { FrameNumber = 12 }));
        Assert.Equal("zoom:anika", Resolve("panel", nextFrame with { SceneId = "panel", FrameNumber = 12 }));
    }

}
