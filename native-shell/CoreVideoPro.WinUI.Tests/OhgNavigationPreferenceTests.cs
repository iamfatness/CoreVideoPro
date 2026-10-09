using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class OhgNavigationPreferenceTests
{
    [Fact]
    public void OlderProfileDefaultsOffAndExplicitChoiceSurvivesRestartSerialization()
    {
        var old = ProductionOutputPreferencesSerializer.Deserialize("{\"Version\":14,\"StreamRtmpEnabled\":false}");
        Assert.NotNull(old);
        Assert.False(old!.OhgShowEnabled);
        old.OhgShowEnabled = true;
        var restored = ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(old));
        Assert.True(restored!.OhgShowEnabled);
        Assert.False(restored.StreamRtmpEnabled);
        restored.OhgShowEnabled = false;
        Assert.False(ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(restored))!.OhgShowEnabled);
    }
    [Fact]
    public void DisablingSelectedOhgReturnsToSettingsAndOtherTabsRemainUsable()
    {
        foreach (var tab in Enum.GetValues<StudioTab>())
        {
            Assert.Equal(tab == StudioTab.OhgShow ? StudioTab.Settings : tab, StudioTabPlumbing.AvailableTab(tab, false));
            Assert.Equal(tab, StudioTabPlumbing.AvailableTab(tab, true));
        }
    }
}
