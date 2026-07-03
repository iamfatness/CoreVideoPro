using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioMultiviewerConfigTests
{
    [Theory]
    [InlineData(0, 1)]
    [InlineData(1, 1)]
    [InlineData(4, 4)]
    [InlineData(8, 8)]
    [InlineData(9, 8)]
    [InlineData(-3, 1)]
    [InlineData(100, 8)]
    public void ClampMultiviewTileCount_ClampsToOneThroughEight(int input, int expected)
    {
        Assert.Equal(expected, StudioViewModel.ClampMultiviewTileCount(input));
    }

    [Theory]
    [InlineData("grid", "grid")]
    [InlineData("pgmPvwTop", "pgmPvwTop")]
    [InlineData("pgmPvwLarge", "pgmPvwLarge")]
    [InlineData("pgmPvwSide", "pgmPvwSide")]
    public void NormalizeMultiviewLayoutMode_KeepsValidModes(string input, string expected)
    {
        Assert.Equal(expected, StudioViewModel.NormalizeMultiviewLayoutMode(input));
    }

    [Theory]
    [InlineData("")]
    [InlineData(null)]
    [InlineData("   ")]
    [InlineData("bogus")]
    [InlineData("GRID")]
    public void NormalizeMultiviewLayoutMode_FallsBackToDefaultForUnknown(string? input)
    {
        // Unknown/blank → the unified-multiviewer default (Program/Preview + source tiles),
        // so a fresh install with no persisted mode opens as the multiviewer, not a bare grid.
        Assert.Equal(StudioViewModel.DefaultMultiviewLayoutMode, StudioViewModel.NormalizeMultiviewLayoutMode(input));
    }

    [Fact]
    public void MultiviewerContractDefaults_MatchSpecification()
    {
        Assert.Equal("pgmPvwLarge", StudioViewModel.DefaultMultiviewLayoutMode);
        Assert.Equal(8, StudioViewModel.DefaultMultiviewTileCount);
        Assert.Equal(1, StudioViewModel.MinMultiviewTileCount);
        Assert.Equal(8, StudioViewModel.MaxMultiviewTileCount);
    }
}
