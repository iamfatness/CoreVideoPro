using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class TilesIdentityPolicyTests
{
    private static TilesIdentityPolicy.Person Guest(string sessionId, string name, string? pid = null) =>
        new($"zoom:{sessionId}", name, pid);

    [Fact]
    public void LiveCase479_LegacySessionIdIsNotAppliedToWhoeverInheritedItNextMeeting()
    {
        // Saved from meeting 97682593786: slot 3 = zoom:33556480 (gone), never-show Jason Bache.
        var settings = new DynamicGallerySettings
        {
            ManualSlots = ["zoom:33556480"],
            ExcludedSourceIds = ["zoom:67109888"],
            BoundMeetingId = "97682593786"
        };
        var nextMeeting = new[]
        {
            Guest("33556480", "Someone else"),
            Guest("16778240", "Jason Bache", "jason-pid")
        };

        Assert.Null(TilesIdentityPolicy.ResolveLiveSourceId(
            "zoom:33556480", nextMeeting, "8916561023", settings.BoundMeetingId));
        Assert.Null(TilesIdentityPolicy.ResolveLiveSourceId(
            "zoom:67109888", nextMeeting, "8916561023", settings.BoundMeetingId));

        var stale = TilesIdentityPolicy.DescribeStale(settings, nextMeeting, "8916561023");
        Assert.Contains(stale, entry => entry.Kind == "slot" && entry.Label.Contains("not in this meeting", StringComparison.Ordinal));
        Assert.Contains(stale, entry => entry.Kind == "never-show");
    }

    [Fact]
    public void PersistPrefersPersistentIdThenDisplayName()
    {
        var roster = new[] { Guest("16778240", "Jamal", "jamal-pid") };
        Assert.Equal("zoom-pid:jamal-pid", TilesIdentityPolicy.Persist("zoom:16778240", roster));
        Assert.Equal("zoom-name:Jamal", TilesIdentityPolicy.Persist("zoom:16778240", [Guest("16778240", "Jamal")]));
        Assert.Equal("capture:cam-1", TilesIdentityPolicy.Persist("capture:cam-1", roster));
    }

    [Fact]
    public void NameKeyFollowsThePersonAcrossSessionIds()
    {
        var saved = TilesIdentityPolicy.Persist("zoom:111", [Guest("111", "Jason Bache")]);
        Assert.Equal("zoom-name:Jason Bache", saved);
        Assert.Equal("zoom:222", TilesIdentityPolicy.ResolveLiveSourceId(
            saved, [Guest("222", "Jason Bache")], "meeting-2", "meeting-1"));
    }

    [Fact]
    public void DuplicateDisplayNamesDoNotStealASlot()
    {
        var roster = new[] { Guest("1", "Alex"), Guest("2", "Alex") };
        Assert.Null(TilesIdentityPolicy.ResolveLiveSourceId(
            "zoom-name:Alex", roster, "m", "m"));
    }

    [Fact]
    public void UnboundLegacySessionIdIsNeverApplied()
    {
        var roster = new[] { Guest("16778240", "David Paskin") };
        Assert.Null(TilesIdentityPolicy.ResolveLiveSourceId(
            "zoom:16778240", roster, "8916561023", boundMeetingId: null));
    }

    [Fact]
    public void AssigningInANewMeetingDoesNotValidateLeftoverSessionIds()
    {
        var settings = new DynamicGallerySettings
        {
            ManualSlots = ["zoom:33556480"],
            BoundMeetingId = "97682593786"
        };
        var nextMeeting = new[] { Guest("33556480", "Someone else"), Guest("222", "Jason Bache") };
        settings.BoundMeetingId = "8916561023";
        settings.ManualSlots.Add(TilesIdentityPolicy.Persist("zoom:222", nextMeeting));
        Assert.Null(TilesIdentityPolicy.ResolveLiveSourceId(
            "zoom:33556480", nextMeeting, "8916561023", settings.BoundMeetingId));
        Assert.Equal("zoom:222", TilesIdentityPolicy.ResolveLiveSourceId(
            "zoom-name:Jason Bache", nextMeeting, "8916561023", settings.BoundMeetingId));
    }

    [Fact]
    public void ClearStaleRemovesMissingSlotsAndNeverShow()
    {
        var settings = new DynamicGallerySettings
        {
            ManualSlots = ["zoom-name:Jamal", "zoom:33556480"],
            ExcludedSourceIds = ["zoom-name:Jason Bache", "zoom-name:Susan"],
            BoundMeetingId = "old"
        };
        var roster = new[] { Guest("9", "Susan") };
        TilesIdentityPolicy.ClearStale(settings, roster, "new");
        Assert.Null(settings.ManualSlots[0]);
        Assert.Null(settings.ManualSlots[1]);
        Assert.Equal(["zoom-name:Susan"], settings.ExcludedSourceIds);
    }
}
