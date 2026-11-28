using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Graphics styles slice 2: the model behind CvColorField (parsing, recent colours and the
/// non-blocking contrast and broadcast checks of spec 4.2).</summary>
public sealed class GraphicsColorFieldTests
{
    [Theory]
    [InlineData("#abc", "#AABBCC", "#AABBCCFF")]
    [InlineData("E63946", "#E63946", "#E63946FF")]
    [InlineData("#0A0F18B3", "#0A0F18", "#0A0F18B3")]
    public void ParsesEveryHexFormAndWritesBothStoredForms(string text, string opaque, string withAlpha)
    {
        Assert.True(GraphicsRgba.TryParse(text, out var value));
        Assert.Equal(opaque, value.Format(alpha: false));
        Assert.Equal(withAlpha, value.Format(alpha: true));
    }

    [Theory] [InlineData("")] [InlineData("#1234")] [InlineData("red")] [InlineData(null)]
    public void RefusesAnythingElse(string? text) => Assert.False(GraphicsRgba.TryParse(text, out _));

    [Fact]
    public void AlphaPercentRoundTrips()
    {
        var value = new GraphicsRgba(10, 20, 30).WithAlphaPercent(70);
        Assert.Equal("#0A141EB3", value.Hex8);
        Assert.Equal(70, value.AlphaPercent);
        Assert.Equal(0, value.WithAlphaPercent(-5).A);
        Assert.Equal(255, value.WithAlphaPercent(250).A);
    }

    [Fact]
    public void RecentsKeepTheLastTwelveDistinctColoursNewestFirst()
    {
        var recents = new GraphicsColorRecents();
        for (var i = 0; i < 14; i++) recents.Add($"#0000{i:X2}");
        recents.Add("#00000d"); // a repeat moves to the front instead of duplicating
        recents.Add("not a colour");
        Assert.Equal(GraphicsColorRecents.Capacity, recents.Items.Count);
        Assert.Equal("#00000DFF", recents.Items[0]);
        Assert.Equal(recents.Items.Count, recents.Items.Distinct().Count());
        Assert.DoesNotContain("#000000FF", recents.Items);
    }

    [Fact]
    public void ContrastBelowFourPointFiveWarnsWithTheRatio()
    {
        var low = GraphicsColorChecks.Evaluate("#777777", new() { TextOnPlate = "#555555" });
        var warning = Assert.Single(low);
        Assert.Equal("contrast", warning.Kind);
        Assert.Contains(":1 — below 4.5:1 on air", warning.Message);
        Assert.Empty(GraphicsColorChecks.Evaluate("#FFFFFF", new() { TextOnPlate = "#172433E6" }));
        // A plate is judged against the text drawn on it.
        Assert.Contains(GraphicsColorChecks.Evaluate("#BBBBBB", new() { TextOnThis = "#FFFFFF" }), w => w.Kind == "contrast");
        Assert.Contains(GraphicsColorChecks.Evaluate("#FFFFFF", new() { PlatelessText = true }), w => w.Message.Contains("video behind it"));
    }

    [Fact]
    public void SaturatedRedOrBlueSmallTextWarnsAndLargeTextDoesNot()
    {
        Assert.Contains(GraphicsColorChecks.Evaluate("#FF0000", new() { TextSizePx = 20 }), w => w.Kind == "broadcast" && w.Message.Contains("red"));
        Assert.Contains(GraphicsColorChecks.Evaluate("#0033FF", new() { TextSizePx = 24 }), w => w.Kind == "broadcast" && w.Message.Contains("blue"));
        Assert.Empty(GraphicsColorChecks.Evaluate("#FF0000", new() { TextSizePx = 42 }));
        Assert.Empty(GraphicsColorChecks.Evaluate("#C06060", new() { TextSizePx = 20 })); // not saturated
        Assert.Empty(GraphicsColorChecks.Evaluate("#00FF00", new() { TextSizePx = 20 })); // green is not flagged
    }

    [Fact]
    public void PureWhiteOverALargeAreaOffersTheCapAndKeepsAlpha()
    {
        var warning = Assert.Single(GraphicsColorChecks.Evaluate("#FFFFFFE6", new() { LargeArea = true }));
        Assert.Equal("#EBEBEBE6", warning.FixValue);
        Assert.Equal("Cap at 235", warning.FixLabel);
        Assert.Empty(GraphicsColorChecks.Evaluate("#FFFFFF", GraphicsColorCheckContext.None));
        Assert.Empty(GraphicsColorChecks.Evaluate("#FFFFFF00", new() { LargeArea = true }));
    }

    [Fact]
    public void OnePixelLinesWarn()
    {
        Assert.Contains(GraphicsColorChecks.Evaluate("#123456", new() { LineWidthPx = 1 }), w => w.Message.Contains("1 px"));
        Assert.Empty(GraphicsColorChecks.Evaluate("#123456", new() { LineWidthPx = 2 }));
        Assert.Empty(GraphicsColorChecks.Evaluate("#123456", new() { LineWidthPx = 0 }));
    }
}
