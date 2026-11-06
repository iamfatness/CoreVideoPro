using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class LegacyLowerThirdVisibilityPolicyTests
{
    [Fact]
    public void RestartingAutoCannotBypassManualHideThroughItsGraphicRow()
    {
        var graphic = new GraphicOverlay { Id = "automation-lower-third", Name = "Test", Position = "bottom", Accent = "#ffffff", Kind = "lower-third", Enabled = true };
        bool Visible(bool suppressed) => StudioViewModel.ShouldEnableProgramLowerThird(false, ProductionMode.SetAndForget, true, suppressed,
            LegacyLowerThirdVisibilityPolicy.HasEnabledGraphic([graphic], suppressed));
        Assert.True(Visible(false));
        graphic.Enabled = false; // Manual Hide disables the existing row.
        Assert.False(Visible(true));
        graphic.Enabled = true; // Restarting Auto reapplies its overlay policy.
        Assert.False(Visible(true));
        Assert.True(Visible(false)); // Deliberate Show releases the manual suppression.
    }
    [Fact]
    public void ExplicitGraphicAndManualModeKeepTheirExistingVisibility()
    {
        var automatic = new GraphicOverlay { Id = "automation-lower-third", Name = "Test", Position = "bottom", Accent = "#ffffff", Kind = "lower-third", Enabled = true };
        var explicitGraphic = new GraphicOverlay { Id = "graphic-user", Name = "Test", Position = "bottom", Accent = "#ffffff", Kind = "lower-third", Enabled = true };
        Assert.True(LegacyLowerThirdVisibilityPolicy.HasEnabledGraphic([automatic], false));
        Assert.True(LegacyLowerThirdVisibilityPolicy.HasEnabledGraphic([automatic, explicitGraphic], true));
        explicitGraphic.Enabled = false;
        Assert.False(LegacyLowerThirdVisibilityPolicy.HasEnabledGraphic([automatic, explicitGraphic], true));
    }
}

