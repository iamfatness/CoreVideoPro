using CoreVideoPro.WinUI.Services;
using Windows.UI;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

// #476 / T3.5. The picker and the hex box are two views of one value, so a
// round trip has to be exact - a picker that shifts the colour every time it is
// opened is worse than no picker at all.
public class HexColorTests
{
    [Theory]
    [InlineData("#44C1A1", 0x44, 0xC1, 0xA1)]
    [InlineData("44C1A1", 0x44, 0xC1, 0xA1)]   // the # is optional
    [InlineData("  #000000  ", 0x00, 0x00, 0x00)]
    [InlineData("#ffffff", 0xFF, 0xFF, 0xFF)]  // lower case parses
    public void ParsesTheShapesAnOperatorActuallyTypes(string text, byte r, byte g, byte b)
    {
        Assert.True(HexColor.TryParse(text, out var color));
        Assert.Equal(Color.FromArgb(255, r, g, b), color);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("#FFF")]        // 3-digit CSS shorthand is NOT supported
    [InlineData("#GGGGGG")]
    [InlineData("#1234567")]
    [InlineData("#80FFFFFF")]   // 8-digit: alpha these fields do not carry
    public void RefusesWhatItCannotMeanExactly(string? text)
    {
        Assert.False(HexColor.TryParse(text, out _));
    }

    // A field left blank or holding junk must keep showing the caller's default
    // rather than snapping to black, which would look like a value the operator
    // chose.
    [Fact]
    public void AnUnparseableValueKeepsTheCallersDefault()
    {
        var fallback = Color.FromArgb(255, 12, 34, 56);
        Assert.Equal(fallback, HexColor.ParseOrDefault("not a colour", fallback));
        Assert.Equal(fallback, HexColor.ParseOrDefault(null, fallback));
    }

    [Fact]
    public void RoundTripsExactly()
    {
        foreach (var text in new[] { "#000000", "#FFFFFF", "#44C1A1", "#0C1118" })
        {
            Assert.True(HexColor.TryParse(text, out var color));
            Assert.Equal(text, HexColor.ToHex(color));
        }
    }

    // Alpha is dropped on the way out, deliberately: the ColorPicker is
    // configured with IsAlphaEnabled=false, and emitting an 8-digit value would
    // write something the scene payload does not read.
    [Fact]
    public void AlphaIsNeverEmitted()
    {
        Assert.Equal("#102030", HexColor.ToHex(Color.FromArgb(0x80, 0x10, 0x20, 0x30)));
    }
}
