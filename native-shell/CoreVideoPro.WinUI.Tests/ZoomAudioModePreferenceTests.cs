using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ZoomAudioModePreferenceTests
{
    [Fact]
    public void Parse_ReadsBothPersistedModes()
    {
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse("programMix"));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("perGuestIso"));
    }

    [Fact]
    public void Parse_IsCaseInsensitiveAndTrims()
    {
        // Hand-edited prefs files are a supported reality; casing is not a contract.
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("perguestiso"));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("PERGUESTISO"));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("  perGuestIso  "));
    }

    [Fact]
    public void Parse_FallsBackToPerGuestIsoForAnythingUnrecognized()
    {
        // v10 product default: ISO stems are independently controllable and
        // automatically reach Program L/R unless programMix is explicit.
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse(null));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse(""));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("   "));
        Assert.Equal(ZoomAudioMode.PerGuestIso, ZoomAudioModePreference.Parse("chaos"));
    }

    [Fact]
    public void Format_EmitsTheDocumentedWireStrings()
    {
        Assert.Equal("programMix", ZoomAudioModePreference.Format(ZoomAudioMode.ProgramMix));
        Assert.Equal("perGuestIso", ZoomAudioModePreference.Format(ZoomAudioMode.PerGuestIso));
    }

    [Fact]
    public void FormatThenParse_RoundTripsEveryMode()
    {
        foreach (var mode in new[] { ZoomAudioMode.ProgramMix, ZoomAudioMode.PerGuestIso })
        {
            Assert.Equal(mode, ZoomAudioModePreference.Parse(ZoomAudioModePreference.Format(mode)));
        }
    }
}
