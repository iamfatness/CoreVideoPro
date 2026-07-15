using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class BrowserOverlayUrlTests
{
    [Fact]
    public void RemovesForcedBackgroundFromH2rOverlayUrl()
    {
        var normalized = StudioViewModel.NormalizeBrowserOverlayUrl(
            "http://192.168.1.50:4001/output/ABCD/?bg=%23000&theme=news");

        Assert.Equal("http://192.168.1.50:4001/output/ABCD/?theme=news", normalized);
    }

    [Fact]
    public void PreservesNonBackgroundQueryAndFragment()
    {
        var normalized = StudioViewModel.NormalizeBrowserOverlayUrl(
            "https://graphics.example/lower-third?channel=1#live");

        Assert.Equal("https://graphics.example/lower-third?channel=1#live", normalized);
    }
}
