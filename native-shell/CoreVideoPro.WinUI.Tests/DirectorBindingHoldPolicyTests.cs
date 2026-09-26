using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.ViewModels.MagicScene;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class DirectorBindingHoldPolicyTests
{
    private static readonly NativeDirectorSlotBinding A = new() { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" };
    private static readonly NativeDirectorSlotBinding B = new() { SlotIndex = 1, PersonId = "b", SourceId = "zoom:b" };
    private static readonly NativeDirectorSlotBinding C = new() { SlotIndex = 2, PersonId = "c", SourceId = "zoom:c" };

    [Fact]
    public void TwoUpDoesNotCollapseUntilSoloCandidatePersistsEightSeconds()
    {
        var hold = new DirectorBindingHoldPolicy();
        var start = DateTimeOffset.UnixEpoch;
        Assert.False(hold.Evaluate("interview", [A, B], "intro", [A], start).Ready);
        Assert.False(hold.Evaluate("interview", [A, B], "intro", [A], start.AddMilliseconds(7999)).Ready);
        Assert.True(hold.Evaluate("interview", [A, B], "intro", [A], start.AddMilliseconds(8000)).Ready);
    }

    [Fact]
    public void NewSecondTalkerAndFollowSpeakerHaveDistinctHolds()
    {
        Assert.Equal(1200, DirectorBindingHoldPolicy.RequiredHoldMs("intro", [A], "interview", [A, B]));
        Assert.Equal(1500, DirectorBindingHoldPolicy.RequiredHoldMs("intro", [A], "intro", [B]));
        Assert.Equal(0, DirectorBindingHoldPolicy.RequiredHoldMs("intro", [A], "speaker-slides", [B]));
    }

    [Fact]
    public void GalleryAddsAfterOneSecondAndDropsAfterSix()
    {
        Assert.Equal(1000, DirectorBindingHoldPolicy.RequiredHoldMs("panel", [A, B], "panel", [A, B, C]));
        Assert.Equal(6000, DirectorBindingHoldPolicy.RequiredHoldMs("panel", [A, B, C], "panel", [A, B]));
    }

    [Fact]
    public void SceneShareHoldsUseEstablishedEnterAndExitDurations()
    {
        Assert.Equal(2.5, DirectorBindingHoldPolicy.RequiredSceneHoldSeconds("intro", "speaker-slides", 4));
        Assert.Equal(3.5, DirectorBindingHoldPolicy.RequiredSceneHoldSeconds("speaker-slides", "intro", 4));
        Assert.Equal(4, DirectorBindingHoldPolicy.RequiredSceneHoldSeconds("intro", "interview", 4));
    }

    [Fact]
    public void ChangingCandidateRestartsHold()
    {
        var hold = new DirectorBindingHoldPolicy();
        var start = DateTimeOffset.UnixEpoch;
        hold.Evaluate("interview", [A, B], "intro", [A], start);
        hold.Evaluate("interview", [A, B], "speaker-slides", [A], start.AddSeconds(2));
        Assert.False(hold.Evaluate("interview", [A, B], "intro", [A], start.AddSeconds(7)).Ready);
    }
}
