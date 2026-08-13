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
    public void Parse_FallsBackToProgramMixForAnythingUnrecognized()
    {
        // THE safety property: an absent (v8 file), empty, or corrupted value must
        // land on the long-standing Z1 topology, never throw and never guess ISO.
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse(null));
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse(""));
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse("   "));
        Assert.Equal(ZoomAudioMode.ProgramMix, ZoomAudioModePreference.Parse("chaos"));
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
