using CoreVideoPro.Control;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class SceneCueWithRoutesArgsTests
{
    [Fact]
    public void TryParseRoutes_ReadsSlotsAndNulls()
    {
        Assert.True(SceneCueWithRoutesArgs.TryParseRoutes(
            """{"ohg-host":1,"ohg-box-1":null}""", out var routes, out var error));
        Assert.Null(error);
        Assert.Equal(1, routes["ohg-host"]);
        Assert.Null(routes["ohg-box-1"]);
    }

    [Fact]
    public void TryParseRoutes_EmptyObjectIsCueOnly()
    {
        Assert.True(SceneCueWithRoutesArgs.TryParseRoutes("{}", out var routes, out _));
        Assert.Empty(routes);
    }

    [Theory]
    [InlineData("")]
    [InlineData("[]")]
    [InlineData("1")]
    [InlineData("{")]
    [InlineData("""{"":1}""")]
    [InlineData("""{"ohg-host":true}""")]
    [InlineData("""{"ohg-host":1.5}""")]
    [InlineData("""{"ohg-host":"2"}""")]
    public void TryParseRoutes_RefusesBadShape(string raw)
    {
        Assert.False(SceneCueWithRoutesArgs.TryParseRoutes(raw, out _, out var error));
        Assert.False(string.IsNullOrWhiteSpace(error));
    }

    [Fact]
    public void Refuse_UnknownSceneChangesNothing()
    {
        var error = SceneCueWithRoutesArgs.Refuse(
            "missing", ["interview"], ["ohg-host"], [1, 2],
            new Dictionary<string, int?> { ["ohg-host"] = 1 });
        Assert.Equal("Unknown scene 'missing'.", error);
    }

    [Fact]
    public void Refuse_UnknownRouteChangesNothing()
    {
        var error = SceneCueWithRoutesArgs.Refuse(
            "interview", ["interview"], ["ohg-host"], [1, 2],
            new Dictionary<string, int?> { ["ohg-box-9"] = 1 });
        Assert.Equal("Unknown route 'ohg-box-9' on scene 'interview'.", error);
    }

    [Fact]
    public void Refuse_UnknownSlotChangesNothing()
    {
        var error = SceneCueWithRoutesArgs.Refuse(
            "interview", ["interview"], ["ohg-host"], [1, 2],
            new Dictionary<string, int?> { ["ohg-host"] = 99 });
        Assert.Equal("Unknown Show Input slot 99 for route 'ohg-host'.", error);
    }

    [Fact]
    public void Refuse_NullSlotIsAllowed()
    {
        Assert.Null(SceneCueWithRoutesArgs.Refuse(
            "interview", ["interview"], ["ohg-host"], [1],
            new Dictionary<string, int?> { ["ohg-host"] = null }));
    }

    [Fact]
    public void Refuse_KnownSceneAndRoutesPass()
    {
        Assert.Null(SceneCueWithRoutesArgs.Refuse(
            "interview", ["interview"], ["ohg-host", "ohg-box-1"], [1, 2],
            new Dictionary<string, int?> { ["ohg-host"] = 1, ["ohg-box-1"] = 2 }));
    }
}
