using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class CaptureDropoutPresentationTests
{
    [Theory]
    [InlineData("live", "producing", "hold", "Live")]
    [InlineData("stalled", "stalled", "hold", "No new frames for 1.5 s — holding last frame")]
    [InlineData("stalled", "stalled", "black", "No new frames for 1.5 s — Program black")]
    [InlineData("lost", "stalled", "hold", "Lost (device removed) — holding, slate in 1.0 s")]
    [InlineData("lost", "stalled", "black", "Lost (device removed) — Program black, slate in 1.0 s")]
    [InlineData("lost", "failed", "hold", "Lost (device removed) — slate on Program")]
    public void FormatsOnlyTheCoreReportedDecision(string liveness, string onAir, string policy, string expected)
    {
        Assert.Equal(expected, CaptureDropoutPresentation.Status(new NativeMediaCoreSourceHealth
        {
            SourceId = "capture:camera",
            Kind = "capture",
            Liveness = liveness,
            OnAir = onAir,
            DropoutPolicy = policy,
            DropoutPolicyApplies = true,
            LastSampleAgeMs = 1500,
            FailInMs = 1000,
            LossReason = "device-removed"
        }));
    }

    [Fact]
    public void OlderOrUnreportedCoreHasNoInferredStatus()
    {
        Assert.Empty(CaptureDropoutPresentation.Status(new NativeMediaCoreSourceHealth
        {
            SourceId = "capture:screen",
            Kind = "capture",
            Health = "stalled",
            SinceLastNewFrameMs = 90000
        }));
    }
    [Fact]
    public void ReassignedSlotCannotKeepThePreviousCamerasPolicyPermission()
    {
        var slot = new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.UvcWebcam, CaptureDeviceId = "first" };
        var editor = new ShowInputSlotViewModel(slot, () => { });
        Assert.True(editor.ShowSourcePolicies);
        Assert.False(editor.CaptureDropoutPolicyApplies);
        editor.ApplyCaptureDropout(new NativeMediaCoreSourceHealth
        {
            SourceId = "capture:first", Kind = "capture", DropoutPolicyApplies = true,
            Liveness = "live", OnAir = "producing"
        });
        Assert.True(editor.CaptureDropoutPolicyApplies);
        Assert.Equal("Live", editor.CaptureDropoutStatusText);

        slot.CaptureDeviceId = "second";
        Assert.False(editor.CaptureDropoutPolicyApplies);
        Assert.Empty(editor.CaptureDropoutStatusText);
    }

}
