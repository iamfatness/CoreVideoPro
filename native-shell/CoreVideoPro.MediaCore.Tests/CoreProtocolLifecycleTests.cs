using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class CoreProtocolLifecycleTests
{
    private static JsonDocument Response(string? lifecycle) => JsonDocument.Parse($$$$"""
        {"ok":true,"snapshot":{"health":null,"profile":null,"routeCount":3,"recording":{
          "sessionId":"take-1","active":true,"status":"recording","writerStatus":"writing",
          "targetFolder":"Recordings","filenamePrefix":"show","format":"mp4","quality":"high",
          "programPath":"show.mp4","totalFramesWritten":100
          {{{{(lifecycle is null ? "" : ",\"lifecycle\":" + lifecycle)}}}}
        }}}
        """);

    [Theory]
    [InlineData("null")]
    [InlineData("{}")]
    [InlineData("17")]
    [InlineData("{\"sessionId\":\"take-1\",\"desiredActive\":true,\"state\":\"future-state\",\"health\":\"healthy\",\"finalized\":false}")]
    public void ExplicitMalformedLifecycleProducesFailureInsteadOfLegacyLive(string lifecycle)
    {
        using var response = Response(lifecycle);
        var snapshot = CoreProtocolParser.TryParseSyncSnapshot(response);
        Assert.NotNull(snapshot);
        Assert.Equal(3, snapshot.RouteCount);
        Assert.NotNull(snapshot.Recording?.Lifecycle);
        Assert.Equal("failed", snapshot.Recording.Lifecycle.State);
        Assert.False(snapshot.Recording.Active);
        Assert.False(OutputLifecycleReadModel.IsRecordingLive(snapshot.Recording));
        Assert.NotNull(snapshot.Recording.Lifecycle.Error);

        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire?.Recording?.Lifecycle);
        Assert.Equal("failed", wire.Recording.Lifecycle.State);
        Assert.False(OutputLifecycleReadModel.IsRecordingLive(wire.Recording));
    }

    [Fact]
    public void MissingLifecycleRetainsLegacyCompatibility()
    {
        using var response = Response(null);
        var snapshot = CoreProtocolParser.TryParseSyncSnapshot(response);
        Assert.NotNull(snapshot?.Recording);
        Assert.Null(snapshot.Recording.Lifecycle);
        Assert.True(OutputLifecycleReadModel.IsRecordingLive(snapshot.Recording));
    }

    [Fact]
    public void CaseInsensitiveDtoFieldsCannotBypassLifecycleValidation()
    {
        using var original = Response("null");
        using var response = JsonDocument.Parse(original.RootElement.GetRawText()
            .Replace("\"recording\":", "\"Recording\":")
            .Replace("\"lifecycle\":", "\"Lifecycle\":"));
        var snapshot = CoreProtocolParser.TryParseSyncSnapshot(response);
        Assert.NotNull(snapshot?.Recording?.Lifecycle);
        Assert.Equal("failed", snapshot.Recording.Lifecycle.State);
        Assert.False(OutputLifecycleReadModel.IsRecordingLive(snapshot.Recording));
    }

    [Fact]
    public void ValidUnknownHealthRemainsUnverified()
    {
        using var response = Response("""
            {"sessionId":"take-1","desiredActive":true,"state":"live","health":"unknown","finalized":false}
            """);
        var snapshot = CoreProtocolParser.TryParseSyncSnapshot(response);
        Assert.NotNull(snapshot?.Recording?.Lifecycle);
        Assert.Equal("unknown", snapshot.Recording.Lifecycle.Health);
        Assert.False(OutputLifecycleReadModel.IsRecordingLive(snapshot.Recording));
    }
}
