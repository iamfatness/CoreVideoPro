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
        Assert.Equal(4, subscriptions.Count);
        Assert.Contains(subscriptions, subscription => subscription["kind"]?.ToString() == "participant-video");
        Assert.Equal(2, subscriptions.Count(subscription => subscription["kind"]?.ToString() == "participant-audio"));
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