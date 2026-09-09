using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public class TilesMembershipPolicyTests
{
    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(" ")]
    public void TakeRefreshWithClearedSelectionLoadsDefaultsWithoutChangingOverrides(string? sourceId)
    {
        var scene = new DynamicGallerySettings { Overrides = new() { ["zoom:a"] = TilesOverridePolicy.Create(.1, .2, .3, .4, 10, 20, 5) } };
        // The previous Program scene becomes Preview during Take, while the
        // member selector may have been cleared by a source-list refresh.
        var empty = TilesOverridePolicy.EditorValues(scene, sourceId);
        Assert.Equal(0, empty.Rect!.X);
        Assert.Equal(.5, empty.Rect.Width);
        Assert.Equal(1, empty.Rect.Height);
        Assert.Equal(0, empty.CropLeftPercent);
        empty.CropLeftPercent = 30;
        Assert.Single(scene.Overrides);
        Assert.Equal(10, TilesOverridePolicy.EditorValues(scene, "zoom:a").CropLeftPercent);
        Assert.Equal(.5, TilesOverridePolicy.EditorValues(null, sourceId).Rect!.Width);
    }

    [Fact]
    public void SwitchingEditorSourceOrSceneLoadsSavedValuesOrDefaultsWithoutMutation()
    {
        var scene = new DynamicGallerySettings { Overrides = new() { ["zoom:a"] = TilesOverridePolicy.Create(.1, .2, .3, .4, 10, 20, 5) } };
        var selected = TilesOverridePolicy.EditorValues(scene, "zoom:a");
        Assert.Equal(.1, selected.Rect!.X);
        Assert.Equal(10, selected.CropLeftPercent);
        selected.CropLeftPercent = 30;
        Assert.Equal(10, scene.Overrides["zoom:a"].CropLeftPercent);
        Assert.Equal(0, TilesOverridePolicy.EditorValues(scene, "zoom:b").CropLeftPercent);
        Assert.Equal(.5, TilesOverridePolicy.EditorValues(new DynamicGallerySettings(), "zoom:a").Rect!.Width);
        Assert.Equal(.5, TilesOverridePolicy.EditorValues(null, "zoom:a").Rect!.Width);
    }
    [Fact]
    public void OverrideRoundTripPreservesPinnedRectCropBackgroundAndDeepClone()
    {
        var settings = new DynamicGallerySettings { BackgroundColor = "#123456", BackgroundSourceId = "zoom:bg",
            Overrides = new() { ["zoom:a"] = TilesOverridePolicy.Create(0, 0, .4, 1, 10, 20, 3) } };
        var persisted = ScenePersistenceService.ToPersisted(settings);
        var restored = ScenePersistenceService.FromPersisted(persisted);
        var clone = restored.Clone();
        clone.Overrides["zoom:a"].CropLeftPercent = 1;
        Assert.Equal(10, restored.Overrides["zoom:a"].CropLeftPercent);
        Assert.Equal(.4, restored.Overrides["zoom:a"].Rect!.Width);
        Assert.Equal("zoom:bg", restored.BackgroundSourceId);
        Assert.Equal("#123456", restored.BackgroundColor);
    }
    [Theory]
    [InlineData(0, 1, 60, 40, 0)]
    [InlineData(.8, .5, 0, 0, 0)]
    [InlineData(0, 1, 0, 0, 1.5)]
    [InlineData(double.NaN, 1, 0, 0, 0)]
    public void InvalidOverrideIsRejectedBeforeMutation(double x, double width, double left, double right, double z) =>
        Assert.Throws<ArgumentException>(() => TilesOverridePolicy.Create(x, 0, width, 1, left, right, z));
    [Fact]
    public void ManualSlotsKeepHolesAndMissingIdentityWithoutPromotingRoster()
    {
        var settings = new DynamicGallerySettings { AutoFill = false, MembershipMode = "manual",
            ManualSlots = ["zoom:a", null, "zoom:absent", "zoom:b", "zoom:a"], ExcludedSourceIds = ["zoom:b"] };
        var expected = new[] { "zoom:a", "", "zoom:absent", "", "" };
        Assert.Equal(expected, TilesMembershipPolicy.Resolve(settings, ["zoom:a", "zoom:c"]));
        Assert.Equal(expected, TilesMembershipPolicy.Resolve(settings, ["zoom:a", "zoom:c", "zoom:absent"]));
    }
    [Fact]
    public void RoutedModeUsesOnlyLiveSourcesAssignedToTheShow()
    {
        var settings = new DynamicGallerySettings
        {
            MembershipMode = "routed",
            ManualSlots = ["zoom:c"]
        };
        Assert.Equal(new[] { "zoom:c", "zoom:a" },
            TilesMembershipPolicy.Resolve(
                settings,
                ["zoom:a", "zoom:b", "zoom:c"],
                ["zoom:a", "zoom:c", "capture:cam"]));
    }
    [Fact]
    public void AutomaticFillRespectsPriorityAssignmentsExclusionsAndLimit()
    {
        var settings = new DynamicGallerySettings { MaxTiles = 3,
            ManualSlots = ["zoom:c"], ExcludedSourceIds = ["zoom:b"] };
        Assert.Equal(new[] { "zoom:c", "zoom:a", "zoom:d" },
            TilesMembershipPolicy.Resolve(settings, ["zoom:a", "zoom:b", "zoom:c", "zoom:d", "zoom:e"]));
    }

    [Fact]
    public void CloneDoesNotAliasMembershipIntent()
    {
        var settings = new DynamicGallerySettings { ManualSlots = ["zoom:a"], ExcludedSourceIds = ["zoom:b"] };
        var copy = settings.Clone();
        copy.ManualSlots[0] = "zoom:c";
        copy.ExcludedSourceIds.Clear();
        Assert.Equal("zoom:a", settings.ManualSlots[0]);
        Assert.Equal("zoom:b", Assert.Single(settings.ExcludedSourceIds));
    }

    [Theory]
    [InlineData(0)] [InlineData(65)] [InlineData(1.5)] [InlineData(double.NaN)]
    public void InvalidApiSlotNeverMutates(double slot)
    {
        var called = false;
        var result = StudioControlSurface.ApplyTilesSlotRequest(slot, "zoom:a", (_, _) => called = true);
        Assert.False(called);
        Assert.NotEqual(CoreVideoPro.Control.ControlInvokeResult.Success, result);
    }

    [Fact]
    public void ApiSlotPassesCanonicalIdentityAndReportsMutationFailure()
    {
        var result = StudioControlSurface.ApplyTilesSlotRequest(64, "zoom:a", (slot, id) =>
        { Assert.Equal(64, slot); Assert.Equal("zoom:a", id); });
        Assert.Equal(CoreVideoPro.Control.ControlInvokeResult.Success, result);
        Assert.NotEqual(CoreVideoPro.Control.ControlInvokeResult.Success,
            StudioControlSurface.ApplyTilesSlotRequest(1, "zoom:a", (_, _) => throw new InvalidOperationException("No Tiles scene")));
    }
}
