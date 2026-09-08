using System.Text.Json;
using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

/// <summary>
/// Plan 7a Task 13 (fix round 1) — <see cref="ShowConfigLooks.PresetsByLookId"/> is what lets
/// <c>OhgHostAdapter</c> answer <c>setPreview({kind:"look"})</c> before the first <c>applyLook</c>
/// has arrived, so its tolerance of a half-written config is load-bearing: it parses an
/// operator-editable document, and a throw here would take the show engine down at launch over a
/// field <see cref="ShowConfigValidator"/> already has a proper message for.
/// </summary>
public sealed class ShowConfigLooksTests
{
    private static JsonElement Engine(string json) => JsonDocument.Parse(json).RootElement;

    [Fact]
    public void MapsEveryLookIdToItsScenePreset()
    {
        var presets = ShowConfigLooks.PresetsByLookId(Engine("""
        {
          "capacity": 10,
          "looks": [
            { "id": "panel", "scenePreset": "panel-scene", "boxes": 2 },
            { "id": "solo", "scenePreset": "solo-scene" }
          ]
        }
        """));

        Assert.Equal(2, presets.Count);
        Assert.Equal("panel-scene", presets["panel"]);
        Assert.Equal("solo-scene", presets["solo"]);
    }

    [Fact]
    public void LookIdsAreOrdinal()
    {
        var presets = ShowConfigLooks.PresetsByLookId(Engine("""
        { "looks": [ { "id": "Panel", "scenePreset": "a" }, { "id": "panel", "scenePreset": "b" } ] }
        """));

        Assert.Equal("a", presets["Panel"]);
        Assert.Equal("b", presets["panel"]);
    }

    /// <summary>A duplicate id keeps the FIRST entry — the one the engine's own ordered look list
    /// resolves to — rather than letting a later duplicate silently win.</summary>
    [Fact]
    public void ADuplicateLookIdKeepsTheFirstEntry()
    {
        var presets = ShowConfigLooks.PresetsByLookId(Engine("""
        { "looks": [ { "id": "panel", "scenePreset": "first" }, { "id": "panel", "scenePreset": "second" } ] }
        """));

        Assert.Equal("first", presets["panel"]);
    }

    [Theory]
    // No looks at all, a non-array looks, and a non-object entry: none of these is this class's
    // error to report, and none of them may throw on the launch path.
    [InlineData("""{ "capacity": 10 }""")]
    [InlineData("""{ "looks": "nope" }""")]
    [InlineData("""{ "looks": [ 7, null, "x" ] }""")]
    // A look missing either half of the pair contributes nothing rather than a half-entry.
    [InlineData("""{ "looks": [ { "id": "panel" } ] }""")]
    [InlineData("""{ "looks": [ { "scenePreset": "panel-scene" } ] }""")]
    [InlineData("""{ "looks": [ { "id": "panel", "scenePreset": 3 } ] }""")]
    [InlineData("""{ "looks": [ { "id": "", "scenePreset": "panel-scene" } ] }""")]
    [InlineData("""{ "looks": [ { "id": "panel", "scenePreset": "" } ] }""")]
    // Not even an object.
    [InlineData("""[ 1, 2 ]""")]
    [InlineData("""null""")]
    public void MalformedOrAbsentLooksYieldAnEmptyMapRatherThanThrowing(string json)
    {
        Assert.Empty(ShowConfigLooks.PresetsByLookId(Engine(json)));
    }

    /// <summary>The good entries in a partly-broken list still make it through — the adapter should
    /// answer for the looks the operator got right.</summary>
    [Fact]
    public void AGoodLookSurvivesABrokenSibling()
    {
        var presets = ShowConfigLooks.PresetsByLookId(Engine("""
        { "looks": [ { "id": "broken" }, { "id": "panel", "scenePreset": "panel-scene" } ] }
        """));

        Assert.Equal("panel-scene", Assert.Single(presets).Value);
    }
}
