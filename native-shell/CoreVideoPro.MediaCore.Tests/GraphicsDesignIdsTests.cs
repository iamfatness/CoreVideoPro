using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsDesignIdsTests
{
    [Fact]
    public void NewTemplateIdSlugsTheNameAndAppendsSixHex() =>
        Assert.Equal("design-panel-name-bar-a1b2c3", GraphicsDesignIds.NewTemplateId("Panel name bar", [], () => "a1b2c3"));
    [Fact]
    public void LongNamesAreTrimmedSoTheIdFitsSixtyFourCharacters()
    {
        var id = GraphicsDesignIds.NewTemplateId(new string('w', 200), [], () => "a1b2c3");
        Assert.True(id.Length <= 64); Assert.EndsWith("-a1b2c3", id); Assert.True(GraphicsDesignIds.IsDesignTemplateId(id));
        var dashed = GraphicsDesignIds.NewTemplateId(new string('w', 46) + " " + new string('v', 40), [], () => "a1b2c3");
        Assert.True(dashed.Length <= 64); Assert.DoesNotContain("--", dashed); Assert.True(GraphicsDesignIds.IsDesignTemplateId(dashed));
    }
    [Fact]
    public void CollisionRetriesWithANewHex()
    {
        var hexes = new Queue<string>(["a1b2c3", "a1b2c3", "d4e5f6"]);
        var id = GraphicsDesignIds.NewTemplateId("Bar", ["design-bar-a1b2c3"], hexes.Dequeue);
        Assert.Equal("design-bar-d4e5f6", id);
    }
    [Theory]
    [InlineData("")]
    [InlineData("!!!")]
    [InlineData("Ünï")]
    public void NamesWithNoUsableCharactersStillYieldAValidId(string name) =>
        Assert.True(GraphicsDesignIds.IsDesignTemplateId(GraphicsDesignIds.NewTemplateId(name, [], () => "a1b2c3")));
    [Fact]
    public void BuiltInIdsAreNeverProducedOrAccepted()
    {
        foreach (var builtIn in new[] { "compact-solid", "minimal-accent", "broadcast", "corporate-lower-third", "logo-accent", "transparent-name", "centered-title", "corner-logo", "clock-panel" })
            Assert.False(GraphicsDesignIds.IsDesignTemplateId(builtIn));
        Assert.Equal("design-broadcast-a1b2c3", GraphicsDesignIds.NewTemplateId("broadcast", [], () => "a1b2c3"));
    }
    [Theory]
    [InlineData("design-x-a1b2c3", true)]
    [InlineData("design-a-b-c-a1b2c3", true)]
    [InlineData("design-x-A1B2C3", false)]
    [InlineData("design-x-a1b2c", false)]
    [InlineData("design-x-a1b2cg", false)]
    [InlineData("designx-a1b2c3", false)]
    [InlineData("design--a1b2c3", false)]
    [InlineData("design-x_y-a1b2c3", false)]
    [InlineData("", false)]
    public void IsDesignTemplateIdChecksShape(string id, bool expected) => Assert.Equal(expected, GraphicsDesignIds.IsDesignTemplateId(id));
    [Fact]
    public void IdsOverSixtyFourCharactersAreRejected() =>
        Assert.False(GraphicsDesignIds.IsDesignTemplateId("design-" + new string('x', 60) + "-a1b2c3"));
}
