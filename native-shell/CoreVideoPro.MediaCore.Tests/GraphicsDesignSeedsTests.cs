using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsDesignSeedsTests
{
    [Fact]
    public void KindsAreTheFourSeeds() =>
        Assert.Equal(["lower-third", "title", "bug", "full-screen"], GraphicsDesignSeeds.Kinds);

    [Theory]
    [InlineData("lower-third", "lower-third", "name,title")]
    [InlineData("title", "title", "heading,subheading")]
    [InlineData("bug", "bug", "logo")]
    [InlineData("full-screen", "title", "heading")]
    public void EachKindStampsIdVersionNameAndKeepsItsFamilyAndFields(string kind, string family, string fields)
    {
        var manifest = GraphicsDesignSeeds.Manifest(kind, "design-my-look-a1b2c3", 4, "My look");
        Assert.Equal("design-my-look-a1b2c3", (string?)manifest["templateId"]);
        Assert.Equal(4, (int?)manifest["templateVersion"]);
        Assert.Equal("My look", (string?)manifest["name"]);
        Assert.Equal(1, (int?)manifest["schemaVersion"]);
        Assert.Equal(family, (string?)manifest["family"]);
        Assert.Equal(1920, (int?)manifest["canvas"]!["width"]);
        Assert.Equal(1080, (int?)manifest["canvas"]!["height"]);
        Assert.Equal(fields.Split(','), manifest["fields"]!.AsArray().Select(f => (string)f!["id"]!));
    }

    [Fact]
    public void ManifestReturnsAnIndependentCopyEachTime()
    {
        var first = GraphicsDesignSeeds.Manifest("bug", "design-a-000001", 1, "A");
        first["name"] = "changed";
        Assert.Equal("B", (string?)GraphicsDesignSeeds.Manifest("bug", "design-b-000002", 1, "B")["name"]);
    }

    [Fact]
    public void UnknownKindThrows() =>
        Assert.Throws<ArgumentException>(() => GraphicsDesignSeeds.Manifest("poster", "design-x-000000", 1, "X"));
}