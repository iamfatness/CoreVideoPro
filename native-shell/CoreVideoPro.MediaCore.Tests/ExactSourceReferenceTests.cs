using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ExactSourceReferenceTests
{
    [Fact]
    public void JsonRoundTripPreservesFullIdentityAndUnknownFields()
    {
        var reference = new ExactSourceReference("source", "instance", "epoch", 9_007_199_254_740_991, "camera");
        var json = JsonSerializer.Serialize(reference);
        Assert.Contains("\"sourceId\"", json);
        Assert.Equal(reference, JsonSerializer.Deserialize<ExactSourceReference>(json));
        Assert.Equal(reference, JsonSerializer.Deserialize<ExactSourceReference>(json[..^1] + ",\"future\":true}"));
    }

    [Fact]
    public void EqualityDistinguishesEveryIdentityDimension()
    {
        var original = new ExactSourceReference("source", "instance", "epoch", 1, "camera");
        Assert.Equal(original, new ExactSourceReference("source", "instance", "epoch", 1, "camera"));
        Assert.NotEqual(original, new ExactSourceReference("source", "instance", "epoch", 1, "share"));
        Assert.NotEqual(original, new ExactSourceReference("source", "replacement", "epoch", 1, "camera"));
        Assert.NotEqual(original, new ExactSourceReference("source", "instance", "replacement", 1, "camera"));
        Assert.NotEqual(original, new ExactSourceReference("source", "instance", "epoch", 2, "camera"));
        Assert.NotEqual(original, new ExactSourceReference("replacement", "instance", "epoch", 1, "camera"));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    [InlineData(9007199254740992)]
    public void RejectsUnsafeGenerations(long generation) =>
        Assert.Throws<ArgumentOutOfRangeException>(() => new ExactSourceReference("s", "i", "e", generation, "camera"));

    [Fact]
    public void RejectsMissingOrMalformedIdentity()
    {
        Assert.Throws<ArgumentException>(() => new ExactSourceReference("", "i", "e", 1, "camera"));
        Assert.Throws<ArgumentException>(() => new ExactSourceReference("s", "", "e", 1, "camera"));
        Assert.Throws<ArgumentException>(() => new ExactSourceReference("s", "i", "", 1, "camera"));
        Assert.Throws<ArgumentException>(() => new ExactSourceReference("s", "i", "e", 1, "person"));
        Assert.Throws<ArgumentException>(() => new ExactSourceReference(new string('é', 257), "i", "e", 1, "camera"));
        Assert.ThrowsAny<ArgumentException>(() => new ExactSourceReference("\ud800", "i", "e", 1, "camera"));
    }

    [Fact]
    public void WireDtoPreservesReferenceAndProductionBuilderRejectsUngatedPin()
    {
        var source = new ExactSourceReference("s", "i", "e", 1, "share");
        var route = new MediaCoreSceneRouteWire("route", "fixed", "mix", "42", ExactSourceRef: source);
        var dtoJson = JsonSerializer.Serialize(route);
        Assert.Contains("\"exactSourceRef\"", dtoJson);
        Assert.Equal(source, JsonSerializer.Deserialize<MediaCoreSceneRouteWire>(dtoJson)!.ExactSourceRef);
        var exception = Assert.Throws<InvalidOperationException>(() =>
            MediaCoreCommandBuilder.BuildSceneGraphCommand("scene", [route]));
        Assert.Contains("exact-source frame routing is not available", exception.Message);
    }
}
