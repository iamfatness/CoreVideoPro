using CoreVideoPro.Control;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlActionRegistryTests
{
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

    [Fact]
    public void TryBind_RejectsMissingRequiredArg()
    {
        Assert.False(ControlActionRegistry.TryBind("scene.select", System.Array.Empty<object?>(), out _, out var error));
        Assert.Contains("sceneId", error);
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
            "scene.dynamicGallery.create",
            "input.assign", "input.name", "graphics.lowerThird.toggle", "audio.zoomMode.set", "audio.monitor.set",
            "multiview.layout.set", "automation.autoAssignInputs.set"
        })
        {
            Assert.True(ControlActionRegistry.Contains(id), $"missing action {id}");
        }
    }
}
