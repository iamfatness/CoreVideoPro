using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomMediaSpinePayloadBuilderTests
{
    [Fact]
    public void BuildRequestsVideoAndAudioSubscriptionsForParticipants()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants =
            [
                new MediaCoreParticipantWire("p1", "Alex", "host", "main", "Main", true, false, false, 70, "live"),
                new MediaCoreParticipantWire("p2", "Sam", "guest", "main", "Main", false, true, false, 0, "live")
            ]
        });

        Assert.False((bool)payload["blocked"]!);
        var participants = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["participants"]);
        Assert.Equal(2, participants.Count);
        var subscriptions = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"]);
        Assert.Equal(5, subscriptions.Count);
        Assert.Contains(subscriptions, subscription =>
            subscription["kind"]?.ToString() == "meeting-audio" &&
            subscription["purpose"]?.ToString() == "program");
        Assert.Contains(subscriptions, subscription => subscription["kind"]?.ToString() == "participant-video");
        Assert.Equal(2, subscriptions.Count(subscription => subscription["kind"]?.ToString() == "participant-audio"));
    }

    [Fact]
    public void BuildRequestsMeetingMixWhenVideoSubscriptionsAreDisabled()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            MaxVideoSubscriptions = 0,
            Participants =
            [
                new MediaCoreParticipantWire("p1", "Alex", "host", "main", "Main", true, false, false, 70, "live")
            ]
        });

        var subscriptions = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"]);
        Assert.Contains(subscriptions, subscription => subscription["kind"]?.ToString() == "meeting-audio");
        Assert.DoesNotContain(subscriptions, subscription => subscription["kind"]?.ToString() == "participant-video");
    }

    [Fact]
    public void BuildWarmsProgramAndPreviewRoutesAheadOfRosterOnlyFeeds()
    {
        var participants = Enumerable.Range(1, 10)
            .Select(index => new MediaCoreParticipantWire(
                $"p{index}", $"Guest {index}", "guest", "main", "Main",
                index == 1, false, false, index == 1 ? 70 : 0, "live"))
            .ToList();
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            MaxVideoSubscriptions = 3,
            Participants = participants,
            ProgramSceneRoutes =
            [
                new MediaCoreSceneRouteWire("program-guest", "fixed", "isolated", "p9")
            ],
            PreviewSceneRoutes =
            [
                new MediaCoreSceneRouteWire("preview-guest", "fixed", "isolated", "p10")
            ]
        });

        var subscriptions = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"]);
        var video = subscriptions
            .Where(subscription => subscription["kind"]?.ToString() == "participant-video")
            .ToList();
        Assert.Equal(3, video.Count);
        Assert.Equal("p1", video[0]["participantId"]);
        Assert.Equal("active-speaker", video[0]["purpose"]);
        Assert.Equal("p9", video[1]["participantId"]);
        Assert.Equal("program", video[1]["purpose"]);
        Assert.Equal("p10", video[2]["participantId"]);
        Assert.Equal("preview", video[2]["purpose"]);
        Assert.DoesNotContain(video, subscription => subscription["participantId"]?.ToString() == "p2");
    }

    [Fact]
    public void BuildOmitsMultiviewWhenLayoutIsNull()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true
        });

        Assert.False(payload.ContainsKey("multiview"));
    }

    [Fact]
    public void BuildCarriesMultiviewLayoutWhenProvided()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Multiview = new MediaCoreMultiviewLayout(
                1920,
                1080,
                2,
                1,
                [
                    new MediaCoreMultiviewSourceWire("participant:p1", "zoom", 0, "Alex", ParticipantId: "p1"),
                    new MediaCoreMultiviewSourceWire("capture:cam1", "capture", 1, "Cam 1", CaptureDeviceId: "cam1")
                ])
        });

        var multiview = Assert.IsType<Dictionary<string, object?>>(payload["multiview"]);
        Assert.Equal(1920, multiview["canvasWidth"]);
        Assert.Equal(1080, multiview["canvasHeight"]);
        Assert.Equal(2, multiview["cols"]);
        Assert.Equal(1, multiview["rows"]);

        var sources = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(multiview["sources"]);
        Assert.Equal(2, sources.Count);
        Assert.Equal("participant:p1", sources[0]["sourceId"]);
        Assert.Equal("zoom", sources[0]["kind"]);
        Assert.Equal("p1", sources[0]["participantId"]);
        Assert.Equal(0, sources[0]["slot"]);
        Assert.Equal("capture", sources[1]["kind"]);
        Assert.Equal("cam1", sources[1]["captureDeviceId"]);
    }

    [Fact]
    public void BuildBlocksWhenEngineIsOff()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = false,
            SdkRuntimeReady = false
        });

        Assert.True((bool)payload["blocked"]!);
        var warnings = Assert.IsAssignableFrom<IReadOnlyList<string>>(payload["warnings"]);
        Assert.Contains(warnings, warning => warning.Contains("Media core is not running", StringComparison.Ordinal));
    }
}
