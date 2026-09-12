using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// #470 / T3.7. Live 2026-09-10: 7 Zoom ISOs armed with Engine off recorded
// nothing, reported nothing, and the operator lost the head of every stream.
public class IsoCapturePreflightTests
{
    [Fact]
    public void ArmedZoomIsosWithCaptureOffAreLoud()
    {
        var warning = IsoCapturePreflight.Describe(isoEnabled: true, zoomIsoSourceCount: 7, rawMediaActive: false);
        Assert.NotNull(warning);
        Assert.Contains("7 Zoom ISO sources", warning);
        // It must say what to do, not just that something is wrong.
        Assert.Contains("Turn Engine on", warning);
    }

    [Fact]
    public void OneSourceReadsAsOneSource()
    {
        var warning = IsoCapturePreflight.Describe(true, 1, false);
        Assert.NotNull(warning);
        Assert.Contains("1 Zoom ISO source ", warning);
    }

    [Theory]
    [InlineData(true, 3, true)]    // capture on: nothing to say
    [InlineData(false, 3, false)]  // ISOs off: the switch is off, not a fault
    [InlineData(true, 0, false)]   // no Zoom ISO sources selected
    public void NothingToSayStaysSilent(bool isoEnabled, int count, bool rawMediaActive)
    {
        Assert.Null(IsoCapturePreflight.Describe(isoEnabled, count, rawMediaActive));
    }

    // UNKNOWN is not a problem. Before the first snapshot we cannot tell
    // capture-off from not-yet-reported, and warning on an unobserved state is
    // how an indicator gets trained into background noise.
    [Fact]
    public void AnUnobservedCaptureStateIsNotAWarning()
    {
        Assert.Null(IsoCapturePreflight.Describe(isoEnabled: true, zoomIsoSourceCount: 7, rawMediaActive: null));
    }
}
