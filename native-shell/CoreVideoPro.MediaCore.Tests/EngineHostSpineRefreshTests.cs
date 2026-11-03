using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class EngineHostSpineRefreshTests
{
    private static readonly ZoomMediaSpinePayloadBuilder.BuildInput Input = new()
    {
        EngineRunning = true, StartCapture = true,
        ProgramSceneRoutes = [new("route", "fixed", "mix", "7")],
        StickyAudioParticipantIds = ["7"]
    };

    private static RawCaptureSnapshot Roster(string epoch, long generation, string capture = "4") => new()
    {
        MeetingState = "in_meeting", RosterEpoch = epoch, RosterRevision = 3, CaptureGeneration = capture,
        Participants = [new() { UserId = "7", DisplayName = "Guest", SourceGeneration = generation, VideoOn = true }]
    };

    [Fact]
    public void RefreshUsesOnlyTheAcceptedCaptureAndCurrentSourceIncarnation()
    {
        var intent = new EngineHostSpineIntent(Input, "a", 3, new Dictionary<string, long> { ["7"] = 1 });
        Assert.Null(EngineHostSpineRefresh.Accept(intent, null));
        Assert.Null(EngineHostSpineRefresh.Accept(intent, new() { MeetingState = "in-meeting", RosterEpoch = "a", RosterRevision = 2, CaptureGeneration = "4" }));
        var refresh = Assert.IsType<EngineHostSpineRefresh>(EngineHostSpineRefresh.Accept(intent,
            new() { MeetingState = "in-meeting", RosterEpoch = "a", RosterRevision = 3, CaptureGeneration = "4" }));
        var payload = Assert.IsType<Dictionary<string, object?>>(refresh.Build(Roster("a", 1)));
        Assert.Contains("captureRefresh", payload.Keys);
        Assert.DoesNotContain("startCapture", payload.Keys);
        Assert.DoesNotContain("multiview", payload.Keys);
        Assert.NotEmpty(Assert.IsType<List<Dictionary<string, object?>>>(payload["participants"]));
        var reused = Assert.IsType<Dictionary<string, object?>>(refresh.Build(Roster("a", 2)));
        Assert.Empty(Assert.IsType<List<Dictionary<string, object?>>>(reused["participants"]));
        Assert.Null(refresh.Build(Roster("b", 1)));
        Assert.Null(refresh.Build(Roster("a", 1, "5")));
    }
}
