using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class BrowserOverlayProgramServiceTests
{
    [Fact]
    public void HidesAuthoredBrowserRouteWhenKeyIsOut()
    {
        var routes = BrowserOverlayProgramService.ApplyOnAirState(
            "scene-1",
            [Route("camera:1", 0), Route("browser:1", 1)],
            new HashSet<string>());

        Assert.Equal(1, routes[0].Opacity);
        Assert.Equal(0, routes[1].Opacity);
    }

    [Fact]
    public void ShowsExistingBrowserRouteWithoutDuplicatingIt()
    {
        var routes = BrowserOverlayProgramService.ApplyOnAirState(
            "scene-1",
            [Route("camera:1", 0), Route("browser:1", 1, opacity: 0)],
            new HashSet<string>(["browser:1"]));

        Assert.Equal(2, routes.Count);
        Assert.Equal(1, routes[1].Opacity);
    }

    [Fact]
    public void ShowsOnlyTopmostDuplicateBrowserRoute()
    {
        var routes = BrowserOverlayProgramService.ApplyOnAirState(
            "scene-1",
            [Route("browser:1", 0), Route("browser:1", 1)],
            new HashSet<string>(["browser:1"]));

        Assert.Equal(0, routes[0].Opacity);
        Assert.Equal(1, routes[1].Opacity);
    }

    [Fact]
    public void AddsMissingOnAirBrowserAsTopmostFullCanvasRoute()
    {
        var routes = BrowserOverlayProgramService.ApplyOnAirState(
            "scene-1",
            [Route("camera:1", 0)],
            new HashSet<string>(["browser:1"]));

        var overlay = Assert.Single(routes, route => route.CaptureDeviceId == "browser:1");
        Assert.Equal(1, overlay.ZIndex);
        Assert.Equal(1, overlay.Opacity);
        Assert.Equal(1, overlay.CanvasRect?.Width);
        Assert.Equal(1, overlay.CanvasRect?.Height);
    }

    private static SourceRoute Route(string captureId, int zIndex, double opacity = 1) => new()
    {
        Id = $"route-{zIndex}",
        Mode = SourceRouteMode.CaptureDevice,
        CaptureDeviceId = captureId,
        AudioRole = SourceAudioRole.Isolated,
        Opacity = opacity,
        ZIndex = zIndex
    };
}
