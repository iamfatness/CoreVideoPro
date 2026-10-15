using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StarterSceneFactoryTests
{
    [Theory]
    [InlineData("solo", 1)]
    [InlineData("two-person", 2)]
    [InlineData("screen-presenter", 2)]
    public void CreatedLookKeepsStableTargetsWhenTheFirstSpeakerArrives(string template, int count)
    {
        var (scene, routes) = StarterSceneFactory.Create("custom-test", template, []);
        Assert.Equal(count, routes.Count);
        Assert.All(routes, route => Assert.NotNull(route.CanvasRect));
        var guest = new Participant { Id = "new-speaker", Name = "Guest", IsActiveSpeaker = true };
        var refreshed = SceneRoutingService.GetRouteDefaults(scene, routes, [guest]);
        Assert.DoesNotContain(refreshed, route => route.Mode == SourceRouteMode.ActiveSpeaker || route.ParticipantId == guest.Id);
        Assert.Equal(template == "two-person" ? new int?[] { 1, 2 } : [1],
            refreshed.Where(route => route.ShowInputSlotNumber.HasValue).Select(route => route.ShowInputSlotNumber).ToArray());
        if (template == "screen-presenter") Assert.Equal(SourceRouteMode.ScreenShare, refreshed[1].Mode);
    }

    [Fact]
    public void MissingRoutesAndNewLayoutPositionsDoNotBecomeAutomaticSpeakerDuplicates()
    {
        var scene = new Scene { Id = "custom-test", Layout = "two-up" };
        var guest = new Participant { Id = "speaker", IsActiveSpeaker = true };
        var fresh = SceneRoutingService.GetRouteDefaults(scene, null, [guest]);
        Assert.Equal(new int?[] { 1, 2 }, fresh.Select(route => route.ShowInputSlotNumber));
        var authored = new SourceRoute { Id = "authored", Mode = SourceRouteMode.Fixed, ParticipantId = "specific-guest",
            CanvasRect = new() { X = .2, Y = .1, Width = .3, Height = .4 }, Opacity = .7, EditorLocked = true };
        var expanded = SceneRoutingService.GetRouteDefaults(scene, [authored], [guest]);
        Assert.Equal("specific-guest", expanded[0].ParticipantId);
        Assert.Equal(.3, expanded[0].CanvasRect!.Width);
        Assert.Equal(.7, expanded[0].Opacity);
        Assert.True(expanded[0].EditorLocked);
        Assert.Equal(2, expanded[1].ShowInputSlotNumber);
    }

    [Fact]
    public void CreatedLookRoundTripsAndUsesUniqueNamesWithoutChangingExistingRoutes()
    {
        var (scene, routes) = StarterSceneFactory.Create("custom-new", "two-person", ["Two people"]);
        Assert.NotEqual("Two people", scene.Name);
        var persisted = ScenePersistenceService.ToPersisted(scene, routes);
        var restored = persisted.Routes.Select(ScenePersistenceService.FromPersisted).ToArray();
        Assert.Equal(new int?[] { 1, 2 }, restored.Select(route => route.ShowInputSlotNumber));
        Assert.All(restored, route => Assert.NotNull(route.CanvasRect));
    }

    [Fact]
    public void UnassignedInputRemainsASelectableNamedPlaceholder()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.Fixed, ShowInputSlotNumber = 2 };
        var slots = new[] { new ShowInputSlot { SlotNumber = 1 }, new ShowInputSlot { SlotNumber = 2 } };
        var layer = new SceneCanvasLayerViewModel(0, route, [], [], slots, [], _ => { });
        var option = Assert.Single(layer.ParticipantOptions.Where(option => option.Value == "input-02"));
        Assert.Contains("Unassigned", option.Label);
        Assert.True(layer.TrySelectSource("input-01"));
        Assert.Equal(1, route.ShowInputSlotNumber);
        Assert.DoesNotContain(SceneRoutingService.GetRouteWarnings(new() { Layout = "two-up" }, [route], []),
            warning => warning.Contains("fixed participant is unavailable"));
    }
}
