using System.Text.Json;
using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public class ShowConfigValidatorTests
{
    private static readonly IReadOnlySet<string> SceneIds = new HashSet<string>(StringComparer.Ordinal)
    {
        "solo-scene", "as-scene", "black-scene", "gallery-scene", "look-a-scene"
    };

    private static JsonElement Engine(string json) => JsonDocument.Parse(json).RootElement.Clone();

    private static ShowPresetScenes AllValidPresets() => new()
    {
        Solo = "solo-scene",
        ActiveSpeaker = "as-scene",
        Black = "black-scene",
        Gallery = "gallery-scene"
    };

    private static ShowConfig MakeConfig(string engineJson, ShowPresetScenes? presets = null, string transition = "cut") => new()
    {
        Engine = Engine(engineJson),
        Shell = new ShowShellConfig { Presets = presets ?? AllValidPresets(), DefaultTransition = transition }
    };

    private static ShowConfig ValidConfig() => MakeConfig("""
        {
          "capacity": 10,
          "looks": [
            { "id": "look-a", "scenePreset": "look-a-scene" }
          ]
        }
        """);

    [Fact]
    public void ValidConfig_ReturnsNull()
    {
        Assert.Null(ShowConfigValidator.Validate(ValidConfig(), SceneIds));
    }

    [Fact]
    public void EngineNotAnObject_ReturnsError()
    {
        var config = new ShowConfig { Engine = JsonDocument.Parse("42").RootElement.Clone() };

        var problem = ShowConfigValidator.Validate(config, SceneIds);

        Assert.Equal("config.engine must be an object", problem);
    }

    [Fact]
    public void CapacityMismatch_ReturnsExactMessage()
    {
        var config = MakeConfig("""{"capacity":9}""");

        var problem = ShowConfigValidator.Validate(config, SceneIds, hostCapacity: 10);

        Assert.Equal("config.capacity must be 10 (the Show Input count); found 9", problem);
    }

    [Fact]
    public void CapacityMissing_ReturnsError()
    {
        var config = MakeConfig("""{}""");

        var problem = ShowConfigValidator.Validate(config, SceneIds);

        Assert.Equal("config.capacity must be 10 (the Show Input count); found <missing>", problem);
    }

    [Fact]
    public void CapacityMatchesCustomHostCapacity_Passes()
    {
        var config = MakeConfig("""{"capacity":5}""");

        var problem = ShowConfigValidator.Validate(config, SceneIds, hostCapacity: 5);

        Assert.Null(problem);
    }

    [Fact]
    public void LookScenePresetMissingFromSceneIds_ReturnsError()
    {
        var config = MakeConfig("""
            {
              "capacity": 10,
              "looks": [ { "id": "look-a", "scenePreset": "nonexistent-scene" } ]
            }
            """);

        var problem = ShowConfigValidator.Validate(config, SceneIds);

        Assert.Equal("look 'look-a' names scene 'nonexistent-scene' which does not exist", problem);
    }

    [Theory]
    [InlineData("solo")]
    [InlineData("activeSpeaker")]
    [InlineData("black")]
    [InlineData("gallery")]
    public void PresetNamesMissingScene_ReturnsError(string presetName)
    {
        const string missing = "nonexistent-scene";
        var presets = AllValidPresets();
        presets = presetName switch
        {
            "solo" => new ShowPresetScenes { Solo = missing, ActiveSpeaker = presets.ActiveSpeaker, Black = presets.Black, Gallery = presets.Gallery },
            "activeSpeaker" => new ShowPresetScenes { Solo = presets.Solo, ActiveSpeaker = missing, Black = presets.Black, Gallery = presets.Gallery },
            "black" => new ShowPresetScenes { Solo = presets.Solo, ActiveSpeaker = presets.ActiveSpeaker, Black = missing, Gallery = presets.Gallery },
            "gallery" => new ShowPresetScenes { Solo = presets.Solo, ActiveSpeaker = presets.ActiveSpeaker, Black = presets.Black, Gallery = missing },
            _ => throw new ArgumentOutOfRangeException(nameof(presetName))
        };

        var config = MakeConfig("""{"capacity":10}""", presets);

        var problem = ShowConfigValidator.Validate(config, SceneIds);

        Assert.Equal($"preset '{presetName}' names scene '{missing}' which does not exist", problem);
    }

    [Fact]
    public void AllPresetsNull_IsAllowed()
    {
        var config = MakeConfig("""{"capacity":10}""", new ShowPresetScenes());

        Assert.Null(ShowConfigValidator.Validate(config, SceneIds));
    }

    [Theory]
    [InlineData("cut")]
    [InlineData("fade")]
    [InlineData("dip")]
    [InlineData("wipe")]
    public void ValidTransitions_Pass(string transition)
    {
        var config = MakeConfig("""{"capacity":10}""", AllValidPresets(), transition);

        Assert.Null(ShowConfigValidator.Validate(config, SceneIds));
    }

    [Fact]
    public void InvalidTransition_ReturnsExactMessage()
    {
        var config = MakeConfig("""{"capacity":10}""", AllValidPresets(), "sting");

        var problem = ShowConfigValidator.Validate(config, SceneIds);

        Assert.Equal("defaultTransition must be one of cut, fade, dip, wipe; found 'sting'", problem);
    }

    [Fact]
    public void FirstProblemWins_CapacityBeforeLooksBeforePresetsBeforeTransition()
    {
        var config = MakeConfig(
            """{"capacity":9,"looks":[{"id":"x","scenePreset":"nope"}]}""",
            new ShowPresetScenes { Solo = "also-nope" },
            "nope-transition");

        var problem = ShowConfigValidator.Validate(config, SceneIds);

        Assert.Equal("config.capacity must be 10 (the Show Input count); found 9", problem);
    }
}
