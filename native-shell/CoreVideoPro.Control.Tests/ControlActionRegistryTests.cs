using CoreVideoPro.Control;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlActionRegistryTests
{
    [Fact]
    public void InvalidDestinationBindingDoesNotEchoMisplacedCredentials()
    {
        Assert.False(ControlActionRegistry.TryBind("transport.stream.rtmp.configure",
            ["secret-sentinel", "rtmps", "rtmps://example.test/live", "key-sentinel"], out _, out var error));
        Assert.Contains("enabled", error);
        Assert.DoesNotContain("sentinel", error);
    }

    [Fact]
    public void Actions_HaveUniqueDotNamespacedIds()
    {
        var ids = ControlActionRegistry.Actions.Select(a => a.Id).ToList();
        Assert.Equal(ids.Count, ids.Distinct(StringComparer.Ordinal).Count());
        Assert.All(ids, id => Assert.Matches("^[a-z][a-zA-Z0-9]*(\\.[a-z][a-zA-Z0-9]*)+$", id));
    }

    [Fact]
    public void TryBind_CoercesArgumentTypes()
    {
        // transport.record.set expects a single bool.
        Assert.True(ControlActionRegistry.TryBind("transport.record.set", new object?[] { 1 }, out var bound, out _));
        Assert.True(Assert.IsType<bool>(bound[0]));

        // input.assign expects (int slot, string sourceId); a float slot coerces to int.
        Assert.True(ControlActionRegistry.TryBind("input.assign", new object?[] { 3.0f, "zoom:p-1" }, out var assign, out _));
        Assert.Equal(3, Assert.IsType<int>(assign[0]));
        Assert.Equal("zoom:p-1", assign[1]);

        // audio.monitor.volume expects a double; an int coerces.
        Assert.True(ControlActionRegistry.TryBind("audio.monitor.volume", new object?[] { 1 }, out var vol, out _));
        Assert.Equal(1.0, Assert.IsType<double>(vol[0]));

        Assert.True(ControlActionRegistry.TryBind("audio.zoomMode.set", new object?[] { "perGuestIso" }, out var zoomMode, out _));
        Assert.Equal("perGuestIso", zoomMode[0]);
    }

    // #1073: the Tiles "name each tile" option beside the other scene.tiles.* actions,
    // one required bool, exposed in the control state as previewTilesNamesOn.
    [Fact]
    public void TilesNamesSet_IsRegisteredWithOneRequiredBoolAndExposedInState()
    {
        Assert.True(ControlActionRegistry.TryGet("scene.tiles.names.set", out var descriptor));
        var parameter = Assert.Single(descriptor.Params);
        Assert.Equal("enabled", parameter.Name);
        Assert.Equal(ControlParamType.Bool, parameter.Type);
        Assert.True(parameter.Required);
        Assert.True(ControlActionRegistry.TryBind("scene.tiles.names.set", new object?[] { 1 }, out var bound, out _));
        Assert.True(Assert.IsType<bool>(bound[0]));
        Assert.False(ControlActionRegistry.TryBind("scene.tiles.names.set", System.Array.Empty<object?>(), out _, out var error));
        Assert.NotNull(error);

        Assert.Contains("previewTilesNamesOn", ControlManifest.StateFields);
        var on = ControlState.Empty with { PreviewTilesNamesOn = true };
        var message = Assert.Single(Osc.OscFeedback.Encode(on, new Osc.OscAddressMap()),
            m => m.Address.EndsWith("/previewTilesNamesOn", StringComparison.Ordinal));
        Assert.Equal(1, Assert.IsType<int>(message.Args[0]));
    }

    [Fact]
    public void TryBind_RejectsMissingRequiredArg()
    {
        Assert.False(ControlActionRegistry.TryBind("scene.select", System.Array.Empty<object?>(), out _, out var error));
        Assert.Contains("sceneId", error);
        Assert.True(ControlActionRegistry.TryBind("scene.cueWithRoutes",
            new object?[] { "interview", """{"ohg-host":1}""" }, out var cue, out _));
        Assert.Equal("interview", cue[0]);
        Assert.Equal("""{"ohg-host":1}""", cue[1]);
        Assert.True(ControlActionRegistry.TryBind("scene.route.set",
            new object?[] { "interview", "ohg-host", 2 }, out var one, out _));
        Assert.Equal(2, one[2]);
    }

    [Fact]
    public void TryBind_AllowsMissingOptionalArg()
    {
        // input.name: (int slot required, string name optional) — omitting the name is a reset.
        Assert.True(ControlActionRegistry.TryBind("input.name", new object?[] { 2 }, out var bound, out _));
        Assert.Equal(2, bound[0]);
        Assert.Null(bound[1]);

        Assert.True(ControlActionRegistry.TryBind("zoom.join", new object?[] { "https://zoom.us/j/123" }, out var join, out _));
        Assert.Equal("https://zoom.us/j/123", join[0]);
        Assert.Null(join[1]);
        Assert.Null(join[2]);

        Assert.True(ControlActionRegistry.TryBind("audio.monitor.set",
            new object?[] { true }, out var legacyMonitor, out _));
        Assert.Equal(4, legacyMonitor.Count);
        Assert.Null(legacyMonitor[1]);
        Assert.True(ControlActionRegistry.TryBind("audio.monitor.volume",
            new object?[] { 0.5, "core-1", 2, "client-a" }, out var versionedMonitor, out _));
        Assert.Equal("core-1", versionedMonitor[1]);
        Assert.Equal(2.0, versionedMonitor[2]);
        Assert.Equal("client-a", versionedMonitor[3]);
    }

    [Fact]
    public void MonitorStateExposesAppliedEpochAndRevisionForSecondClient()
    {
        var state = new ControlState
        {
            AudioMonitorOn = true, AudioMonitorVolume = 0.5,
            AudioMonitorAuthorityEpoch = "core-1", AudioMonitorRevision = 3,
            AudioMonitorLastResult = "applied"
        };
        var json = System.Text.Json.JsonSerializer.Serialize(state);
        using var document = System.Text.Json.JsonDocument.Parse(json);
        Assert.Equal("core-1", document.RootElement.GetProperty("AudioMonitorAuthorityEpoch").GetString());
        Assert.Equal(3, document.RootElement.GetProperty("AudioMonitorRevision").GetInt64());
    }

    [Fact]
    public void AudioRouteActionRequiresRevisionAndStatePublishesAppliedSends()
    {
        Assert.False(ControlActionRegistry.TryBind("audio.route.set",
            new object?[] { "zoom:42", "mon", true, -6 }, out _, out _));
        Assert.True(ControlActionRegistry.TryBind("audio.route.set",
            new object?[] { "zoom:42", "mon", true, -6, "core-1", 2, "client-b" },
            out var bound, out _));
        Assert.Equal(2.0, bound[5]);
        var state = new ControlState
        {
            AudioRouteAuthorityEpoch = "core-1", AudioRouteRevision = 3,
            AudioRoutes = [new ControlAudioRouteState("zoom:42", "mon", -6)]
        };
        var json = System.Text.Json.JsonSerializer.Serialize(state);
        using var document = System.Text.Json.JsonDocument.Parse(json);
        Assert.Equal(3, document.RootElement.GetProperty("AudioRouteRevision").GetInt64());
        Assert.Equal("zoom:42", document.RootElement.GetProperty("AudioRoutes")[0]
            .GetProperty("SourceId").GetString());
    }

    [Fact]
    public void TryBind_RejectsUnknownAction()
    {
        Assert.False(ControlActionRegistry.TryBind("bogus.action", System.Array.Empty<object?>(), out _, out var error));
        Assert.Contains("Unknown action", error);
    }

    [Fact]
    public void CoreActions_ArePresent()
    {
        foreach (var id in new[]
        {
            "zoom.join", "zoom.leave", "transport.take", "transport.record.toggle", "transport.stream.set", "scene.select",
            "scene.cueWithRoutes", "scene.route.set",
            "scene.dynamicGallery.create",
            "input.assign", "input.name", "source.dropout.set", "graphics.lowerThird.toggle", "audio.zoomMode.set", "audio.monitor.set",
            "multiview.layout.set", "automation.autoAssignInputs.set"
        })
        {
            Assert.True(ControlActionRegistry.Contains(id), $"missing action {id}");
        }
    }

    [Theory]
    [InlineData("program", "program")]
    [InlineData("preview", "preview")]
    [InlineData("program-preview", "program-preview")]
    [InlineData("programPreview", "program-preview")]
    [InlineData("multiview", "multiview")]
    public void ViewMode_AcceptsAdvertisedModesAndLegacyAlias(string requested, string canonical)
    {
        Assert.True(ViewModeContract.TryNormalize(requested, out var actual));
        Assert.Equal(canonical, actual);
    }

    [Theory]
    [InlineData("")]
    [InlineData("Program")]
    [InlineData("programPreviewWrong")]
    public void ViewMode_RejectsUnsupportedValue(string requested)
    {
        Assert.False(ViewModeContract.TryNormalize(requested, out _));
    }
}
