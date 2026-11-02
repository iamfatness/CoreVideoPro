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
            NdiOutputEnabled = true,
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
            $"{{\"Version\":{ProductionOutputPreferences.CurrentVersion},\"NdiOutputEnabled\":true}}", out var migrated);
        Assert.NotNull(loaded);
        Assert.False(migrated);
        Assert.True(loaded!.NdiOutputEnabled);
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

    // Review 4A: a key the core cannot carry (or with a bad name) is DROPPED from the destination
    // list on its own; it is never a stream validation error, so it cannot stop RTMP/SRT/Program NDI.
    [Fact]
    public void KeyThatCannotBeSentIsDroppedAloneAndNeverFailsTheStream()
    {
        Assert.True(StudioStreamOutputValidation.IncludeNdiGraphicsKeyDestination(true, true, true, "Program", null));
        Assert.True(StudioStreamOutputValidation.IncludeNdiGraphicsKeyDestination(true, true, null, "Program", null));
        Assert.False(StudioStreamOutputValidation.IncludeNdiGraphicsKeyDestination(true, true, false, "Program", null));
        Assert.False(StudioStreamOutputValidation.IncludeNdiGraphicsKeyDestination(false, true, true, "Program", null));
        Assert.False(StudioStreamOutputValidation.IncludeNdiGraphicsKeyDestination(true, false, true, "Program", null));
        Assert.False(StudioStreamOutputValidation.IncludeNdiGraphicsKeyDestination(true, true, true, "Program", "Program"));
        // Stream validation knows nothing about the key: a valid NDI selection stays valid whatever the key state.
        Assert.Null(StudioStreamOutputValidation.ValidateSelectedDestinations(
            false, true, false, "rtmps", "", "", "Program", "caller", "", "9000", "120", "", "0", ""));
    }

    // Review 4A/4B: the action refuses (Fail, nothing applied) a key the core cannot carry or that
    // has nothing to ride on because NDI output is off.
    [Theory]
    [InlineData(false, true, "NDI output")]
    [InlineData(true, false, "ndi-graphics-output")]
    public void ActionRefusesMissingCapabilityOrNdiOffAndAppliesNothing(bool ndiEnabled, bool capability, string reason)
    {
        var applied = 0;
        var result = NdiGraphicsKeyControl.Set(true, null, ndiEnabled, capability, "Program", "", (_, _) => applied++);
        Assert.False(result.Ok);
        Assert.Contains(reason, result.Error);
        Assert.Equal(0, applied);
    }

    // Review 4C: on + name is ONE edit: a single apply carrying both, so the caller saves and
    // syncs once instead of two concurrent syncs flapping the key sender.
    [Fact]
    public void ActionAppliesToggleAndNameAsOneEdit()
    {
        var calls = new List<(bool On, string? Name)>();
        var result = NdiGraphicsKeyControl.Set(true, "  Studio Key ", true, true, "Program", "", (on, name) => calls.Add((on, name)));
        Assert.True(result.Ok);
        var call = Assert.Single(calls);
        Assert.True(call.On);
        Assert.Equal("Studio Key", call.Name);

        calls.Clear();
        Assert.True(NdiGraphicsKeyControl.Set(false, null, false, false, "Program", "Studio Key", (on, name) => calls.Add((on, name))).Ok);
        Assert.Equal((false, (string?)null), Assert.Single(calls));  // off is always allowed; name kept
    }

    // Review 5: the advertised name follows the native sender's rules and never equals Program's.
    [Fact]
    public void KeyNameIsValidatedForLengthReservedCharactersAndCollision()
    {
        var longProgram = new string('P', 70);
        var defaultName = StudioStreamOutputValidation.ResolveNdiGraphicsKeyName(longProgram, null);
        Assert.True(defaultName.Length <= StudioStreamOutputValidation.NdiMaxSourceNameLength);
        Assert.EndsWith(" Graphics", defaultName);
        Assert.Null(StudioStreamOutputValidation.ValidateNdiGraphicsKeyName(longProgram, null));

        Assert.Contains("64", StudioStreamOutputValidation.ValidateNdiGraphicsKeyName("Program", new string('k', 65)));
        Assert.Contains("characters NDI does not allow", StudioStreamOutputValidation.ValidateNdiGraphicsKeyName("Program", "Key:One"));
        Assert.Contains("differ", StudioStreamOutputValidation.ValidateNdiGraphicsKeyName("CoreVideo Pro Program", " corevideo pro program "));
        Assert.Null(StudioStreamOutputValidation.ValidateNdiGraphicsKeyName("Program", "Studio Key"));

        var applied = 0;
        var refused = NdiGraphicsKeyControl.Set(false, "Program", true, true, "Program", "", (_, _) => applied++);
        Assert.False(refused.Ok);  // a bad name is refused even when turning the key off
        Assert.Equal(0, applied);
    }

    [Fact]
    public void DestinationIdMatchesTheNativeCore()
    {
        // native/src/modules/GraphicsKeyPixels.h kNdiGraphicsKeyDestination
        Assert.Equal("ndi-graphics", StudioStreamOutputValidation.NdiGraphicsKeyDestination);
    }
}
