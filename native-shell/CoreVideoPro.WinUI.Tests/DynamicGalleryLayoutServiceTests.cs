using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class DynamicGalleryLayoutServiceTests
{
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(5)]
    [InlineData(8)]
    [InlineData(16)]
    public void LayoutIsBoundedUniformAndUsesRequestedAspect(int count)
    {
        var rects = DynamicGalleryLayoutService.BuildRects(count, tileAspectPreset: "4:3");

        Assert.Equal(count, rects.Count);
        var first = rects[0];
        foreach (var rect in rects)
        {
            Assert.InRange(rect.X, 0, 1);
            Assert.InRange(rect.Y, 0, 1);
            Assert.InRange(rect.X + rect.Width, 0, 1.000001);
            Assert.InRange(rect.Y + rect.Height, 0, 1.000001);
            Assert.Equal(first.Width, rect.Width, 6);
            Assert.Equal(first.Height, rect.Height, 6);
            Assert.Equal(4.0 / 3.0, rect.Width * (16.0 / 9.0) / rect.Height, 5);
        }
    }

    [Fact]
    public void ShortFinalRowIsCentered()
    {
        var rects = DynamicGalleryLayoutService.BuildRects(5);
        var lastRowY = rects.Max(rect => rect.Y);
        var lastRow = rects.Where(rect => Math.Abs(rect.Y - lastRowY) < 0.000001).ToList();

        Assert.Equal(2, lastRow.Count);
        var leftMargin = lastRow[0].X;
        var rightMargin = 1 - (lastRow[^1].X + lastRow[^1].Width);
        Assert.Equal(leftMargin, rightMargin, 6);
    }

    [Fact]
    public void EmptyGalleryProducesNoRects()
    {
        Assert.Empty(DynamicGalleryLayoutService.BuildRects(0));
    }
}
