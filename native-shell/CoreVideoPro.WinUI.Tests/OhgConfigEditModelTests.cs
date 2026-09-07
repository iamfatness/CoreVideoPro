using System.Text.Json;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Plan 7b Task 9 — the pure edit-model half: <see cref="OhgConfigEditModel"/> round
/// trips every known <c>engine</c> field through <c>FromConfig</c>/<c>ToConfig</c> and preserves
/// unknown ones (including <c>statePath</c>, which this model deliberately does not own) in
/// <see cref="OhgConfigEditModel.Extra"/>.</summary>
public sealed class OhgConfigEditModelTests
{
    private static JsonElement Json(string json) => JsonDocument.Parse(json).RootElement.Clone();

    private static ShowConfig FullConfig() => new()
    {
        Version = 1,
        Engine = Json("""
        {
          "capacity": 10,
          "utilityPinBase": 9000,
          "galleryCells": 20,
          "skipRoles": ["aslinterpreter", "aslpanelist"],
          "integrations": {"registry": true, "handsQueue": true, "questionFeed": false},
          "mukana": {
            "baseUrl": "https://host/php-panel-rest.php",
            "event": "officehours",
            "panelistsIntervalMs": 4000,
            "handsIntervalMs": 1500,
            "questionIntervalMs": 2500,
            "maxBackoffMs": 45000
          },
          "statePath": "C:\\state\\ohg-show-state.json",
          "looks": [
            {"id":"hr-q","label":"HR + Q","scenePreset":"scene-a","boxes":1,"includesHost":true,"includesReader":false,"plateTone":"neutral","tallySource":"boxes","boxFill":"queue"},
            {"id":"banter","label":"Banter","scenePreset":"scene-b","boxes":2,"includesHost":false,"includesReader":false,"plateTone":"accent","tallySource":"boxes","boxFill":"manual"}
          ],
          "futureField": {"nested": [1, 2, 3]}
        }
        """),
        Shell = new ShowShellConfig
        {
            DriveHost = true,
            Presets = new ShowPresetScenes { Solo = "scene-a", ActiveSpeaker = "scene-b", Black = "scene-c", Gallery = "scene-d" },
            DefaultTransition = "fade",
            TallyUrl = "http://tally.example/"
        }
    };

    [Fact]
    public void FromConfig_ReadsEveryKnownField()
    {
        var model = OhgConfigEditModel.FromConfig(FullConfig(), out var warnings);

        Assert.Empty(warnings);
        Assert.Equal(10, model.Capacity);
        Assert.Equal(9000, model.UtilityPinBase);
        Assert.Equal(20, model.GalleryCells);
        Assert.Equal(["aslinterpreter", "aslpanelist"], model.SkipRoles);
        Assert.True(model.RegistryEnabled);
        Assert.True(model.HandsQueueEnabled);
        Assert.False(model.QuestionFeedEnabled);
        Assert.Equal("https://host/php-panel-rest.php", model.MukanaBaseUrl);
        Assert.Equal("officehours", model.MukanaEvent);
        Assert.Equal(4000, model.PanelistsIntervalMs);
        Assert.Equal(1500, model.HandsIntervalMs);
        Assert.Equal(2500, model.QuestionIntervalMs);
        Assert.Equal(45000, model.MaxBackoffMs);
        Assert.Equal(2, model.Looks.Count);
        Assert.Equal("hr-q", model.Looks[0].Id);
        Assert.Equal("scene-a", model.Looks[0].ScenePreset);
        Assert.Equal("manual", model.Looks[1].BoxFill);

        Assert.True(model.DriveHost);
        Assert.Equal("scene-a", model.PresetSolo);
        Assert.Equal("scene-d", model.PresetGallery);
        Assert.Equal("fade", model.DefaultTransition);
        Assert.Equal("http://tally.example/", model.TallyUrl);

        Assert.True(model.Extra.ContainsKey("statePath"));
        Assert.True(model.Extra.ContainsKey("futureField"));
    }

    [Fact]
    public void RoundTrip_PreservesEveryFieldAndAnUnknownExtra()
    {
        var original = OhgConfigEditModel.FromConfig(FullConfig(), out _);

        var rebuilt = OhgConfigEditModel.FromConfig(original.ToConfig(), out var warnings);

        Assert.Empty(warnings);
        Assert.Equal(original.Capacity, rebuilt.Capacity);
        Assert.Equal(original.UtilityPinBase, rebuilt.UtilityPinBase);
        Assert.Equal(original.GalleryCells, rebuilt.GalleryCells);
        Assert.Equal(original.SkipRoles, rebuilt.SkipRoles);
        Assert.Equal(original.RegistryEnabled, rebuilt.RegistryEnabled);
        Assert.Equal(original.HandsQueueEnabled, rebuilt.HandsQueueEnabled);
        Assert.Equal(original.QuestionFeedEnabled, rebuilt.QuestionFeedEnabled);
        Assert.Equal(original.MukanaBaseUrl, rebuilt.MukanaBaseUrl);
        Assert.Equal(original.MukanaEvent, rebuilt.MukanaEvent);
        Assert.Equal(original.PanelistsIntervalMs, rebuilt.PanelistsIntervalMs);
        Assert.Equal(original.Looks.Count, rebuilt.Looks.Count);
        Assert.Equal(original.Looks[0].Id, rebuilt.Looks[0].Id);
        Assert.Equal(original.Looks[1].ScenePreset, rebuilt.Looks[1].ScenePreset);
        Assert.Equal(original.DriveHost, rebuilt.DriveHost);
        Assert.Equal(original.PresetSolo, rebuilt.PresetSolo);
        Assert.Equal(original.DefaultTransition, rebuilt.DefaultTransition);
        Assert.Equal(original.TallyUrl, rebuilt.TallyUrl);

        // The unknown extra field survives the round trip verbatim.
        Assert.True(rebuilt.Extra.TryGetValue("futureField", out var future));
        Assert.Equal(JsonValueKind.Object, future.ValueKind);
        Assert.Equal(3, future.GetProperty("nested").GetArrayLength());

        // statePath — deliberately not owned by this model — also survives.
        Assert.True(rebuilt.Extra.TryGetValue("statePath", out var statePath));
        Assert.Equal("C:\\state\\ohg-show-state.json", statePath.GetString());
    }

    [Fact]
    public void ToConfig_MukanaIsNullWhenEveryIntegrationIsOff()
    {
        var model = OhgConfigEditModel.Default();
        model.RegistryEnabled = false;
        model.HandsQueueEnabled = false;
        model.QuestionFeedEnabled = false;

        var config = model.ToConfig();

        Assert.True(config.Engine.TryGetProperty("mukana", out var mukana));
        Assert.Equal(JsonValueKind.Null, mukana.ValueKind);
    }

    [Fact]
    public void ToConfig_MukanaIsAnObjectWhenAnyIntegrationIsOn()
    {
        var model = OhgConfigEditModel.Default();
        model.HandsQueueEnabled = true;
        model.MukanaBaseUrl = "https://host/rest";
        model.MukanaEvent = "officehours";

        var config = model.ToConfig();

        Assert.True(config.Engine.TryGetProperty("mukana", out var mukana));
        Assert.Equal(JsonValueKind.Object, mukana.ValueKind);
        Assert.Equal("https://host/rest", mukana.GetProperty("baseUrl").GetString());
    }

    [Fact]
    public void Default_HasTheFourSpecLooksWithEmptyScenePresets()
    {
        var model = OhgConfigEditModel.Default();

        Assert.Equal(10, model.Capacity);
        Assert.Equal(9000, model.UtilityPinBase);
        Assert.Equal(16, model.GalleryCells);
        Assert.Equal(["aslinterpreter"], model.SkipRoles);
        Assert.False(model.DriveHost);
        Assert.Null(model.MukanaBaseUrl);

        Assert.Equal(4, model.Looks.Count);
        Assert.Equal(["hr-q", "banter", "teatime", "panel-checks"], model.Looks.Select(l => l.Id));
        Assert.All(model.Looks, look => Assert.True(string.IsNullOrEmpty(look.ScenePreset)));

        var hrQ = model.Looks.Single(l => l.Id == "hr-q");
        Assert.Equal("HR + Q", hrQ.Label);
        Assert.Equal(1, hrQ.Boxes);
        Assert.True(hrQ.IncludesHost);
        Assert.False(hrQ.IncludesReader);

        var teatime = model.Looks.Single(l => l.Id == "teatime");
        Assert.Equal(3, teatime.Boxes);
        Assert.True(teatime.IncludesReader);

        var panelChecks = model.Looks.Single(l => l.Id == "panel-checks");
        Assert.Equal(4, panelChecks.Boxes);
        Assert.Equal("activeSpeaker", panelChecks.TallySource);
    }

    [Fact]
    public void FromConfig_ReportsWarningsForMalformedKnownFields()
    {
        var config = new ShowConfig
        {
            Version = 1,
            Engine = Json("""{"capacity": "not-a-number", "galleryCells": 16}"""),
            Shell = new ShowShellConfig()
        };

        var model = OhgConfigEditModel.FromConfig(config, out var warnings);

        Assert.NotEmpty(warnings);
        Assert.Contains(warnings, w => w.Contains("capacity"));
        Assert.Equal(10, model.Capacity); // kept the field default
    }
}
