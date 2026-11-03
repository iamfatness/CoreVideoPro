using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>NDI output on the control API (spec 2026-11-01): transport.ndi.set / toggle answer
/// with SetNdiOutput's refusal, and the status label notifies only when its text changes.</summary>
public sealed class NdiOutputActionTests
{
    [Fact]
    public void RefusalIsAFailureCarryingTheReason()
    {
        var result = StudioControlSurface.NdiSetResult("NDI needs a program name");
        Assert.False(result.Ok);
        Assert.Equal("NDI needs a program name", result.Error);
    }

    [Fact]
    public void NoRefusalIsSuccess()
    {
        Assert.True(StudioControlSurface.NdiSetResult(null).Ok);
    }

    [Fact]
    public void StatusLabelNotifiesOnlyWhenTheTextChanges()
    {
        string? last = null;
        Assert.True(StudioViewModel.AdvanceNdiStatusLabel(ref last, "Off"));
        Assert.False(StudioViewModel.AdvanceNdiStatusLabel(ref last, "Off"));
        Assert.True(StudioViewModel.AdvanceNdiStatusLabel(ref last, "Live · Studio A"));
        Assert.Equal("Live · Studio A", last);
        Assert.False(StudioViewModel.AdvanceNdiStatusLabel(ref last, "Live · Studio A"));
        Assert.True(StudioViewModel.AdvanceNdiStatusLabel(ref last, "Live · Studio B"));
    }
}
