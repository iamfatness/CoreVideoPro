using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>NDI graphics key (titler spec Phase 1, "NDI fill+key"): the shell's half — the
/// control action, the persisted preference, the advertised name and the capability gate.</summary>
public sealed class NdiGraphicsKeyOutputTests
{
    [Fact]
    public void ControlActionIsRegisteredAndHandledByTheStudioSurface()
    {
        Assert.True(ControlActionRegistry.TryGet("transport.stream.ndi.graphicsKey.set", out _));
        Assert.Contains("transport.stream.ndi.graphicsKey.set", StudioControlSurface.SupportedActionIds);
    }

    [Fact]
    public void ControlActionBindsOnAndAnOptionalName()
    {
        Assert.True(ControlActionRegistry.TryBind("transport.stream.ndi.graphicsKey.set", [true], out var onOnly, out _));
        Assert.True(Assert.IsType<bool>(onOnly[0]));
        Assert.Null(onOnly[1]);
        Assert.True(ControlActionRegistry.TryBind("transport.stream.ndi.graphicsKey.set", [0, "Studio Key"], out var named, out _));
        Assert.False(Assert.IsType<bool>(named[0]));
        Assert.Equal("Studio Key", named[1]);
        Assert.False(ControlActionRegistry.TryBind("transport.stream.ndi.graphicsKey.set", [], out _, out var error));
        Assert.Contains("on", error);
    }

    [Fact]
    public void PreferencesRoundTripTheKeyToggleAndName()
    {
        var json = ProductionOutputPreferencesSerializer.Serialize(new ProductionOutputPreferences
        {
            StreamNdiEnabled = true,
            StreamNdiGraphicsKeyEnabled = true,
            StreamNdiGraphicsName = "Studio Key",
        });
        var loaded = ProductionOutputPreferencesSerializer.Deserialize(json, out _);
        Assert.NotNull(loaded);
        Assert.True(loaded!.StreamNdiGraphicsKeyEnabled);
        Assert.Equal("Studio Key", loaded.StreamNdiGraphicsName);
    }

    [Fact]
    public void PreferencesWithoutTheKeyFieldsLoadWithTheKeyOff()
    {
        var loaded = ProductionOutputPreferencesSerializer.Deserialize(
            $"{{\"Version\":{ProductionOutputPreferences.CurrentVersion},\"StreamNdiEnabled\":true}}", out var migrated);
        Assert.NotNull(loaded);
        Assert.False(migrated);
        Assert.True(loaded!.StreamNdiEnabled);
        Assert.False(loaded.StreamNdiGraphicsKeyEnabled);
        Assert.Null(loaded.StreamNdiGraphicsName);
    }

    [Theory]
    [InlineData("CoreVideo Pro Program", null, "CoreVideo Pro Program Graphics")]
    [InlineData("CoreVideo Pro Program", "  ", "CoreVideo Pro Program Graphics")]
    [InlineData("  Studio A  ", "", "Studio A Graphics")]
    [InlineData("Studio A", "  Downstream Key ", "Downstream Key")]
    [InlineData(null, null, "CoreVideo Pro Program Graphics")]
    public void GraphicsKeyNameDefaultsToProgramNamePlusGraphics(string? program, string? graphics, string expected)
    {
        Assert.Equal(expected, StudioStreamOutputValidation.ResolveNdiGraphicsKeyName(program, graphics));
    }

    [Fact]
    public void CapabilityGateRefusesOnlyASelectedKeyTheCoreCannotCarry()
    {
        Assert.Null(StudioStreamOutputValidation.ValidateNdiGraphicsKeyCapability(false, false));
        Assert.Null(StudioStreamOutputValidation.ValidateNdiGraphicsKeyCapability(true, true));
        Assert.Null(StudioStreamOutputValidation.ValidateNdiGraphicsKeyCapability(true, null));
        Assert.Contains("ndi-graphics-output", StudioStreamOutputValidation.ValidateNdiGraphicsKeyCapability(true, false));
    }

    [Fact]
    public void DestinationIdMatchesTheNativeCore()
    {
        // native/src/modules/GraphicsKeyPixels.h kNdiGraphicsKeyDestination
        Assert.Equal("ndi-graphics", StudioStreamOutputValidation.NdiGraphicsKeyDestination);
    }
}
