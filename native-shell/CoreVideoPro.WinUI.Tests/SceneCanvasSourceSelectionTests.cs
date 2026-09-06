using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SceneCanvasSourceSelectionTests
{
    private static readonly Participant[] Participants =
    [new() { Id = "jamal", Name = "Jamal" }, new() { Id = "other", Name = "Other guest" }];

    private static SceneCanvasLayerViewModel Model(SourceRoute route, Action<SceneCanvasLayerViewModel>? changed = null) =>
        new(0, route, Participants, [], [], [], changed ?? (_ => { }));

    [Fact]
    public void ClickingAnAutomaticLayerDoesNotPinTheFirstRosterParticipant()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.ActiveSpeaker };
        var layer = Model(route);
        layer.ApplyRoute(); // Pointer release commits even when the click did not drag.
        Assert.Equal(SourceRouteMode.ActiveSpeaker, route.Mode);
        Assert.Null(route.ParticipantId);
        Assert.Equal(string.Empty, layer.ParticipantId);
    }

    [Fact]
    public void GeometryEditDoesNotFillAnUnassignedFixedSource()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.Fixed };
        var layer = Model(route);
        layer.SetCanvasRect(0.1, 0.1, 0.5, 0.5);
        Assert.True(string.IsNullOrEmpty(route.ParticipantId));
        Assert.NotEqual("jamal", layer.ParticipantId);
    }
    [Fact]
    public void ExplicitAutomaticModeClearsOldPinButExplicitPersonStillPins()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.Fixed, ParticipantId = "other" };
        var layer = Model(route);
        layer.Mode = "active-speaker";
        Assert.Equal(SourceRouteMode.ActiveSpeaker, route.Mode);
        Assert.Null(route.ParticipantId);
        layer.ParticipantId = "other";
        Assert.Equal(SourceRouteMode.Fixed, route.Mode);
        Assert.Equal("other", route.ParticipantId);
    }

    [Fact]
    public void SnapshotRefreshPreservesAutomaticRouteAndStableOptions()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.ActiveSpeaker };
        var changes = 0;
        var layer = Model(route, _ => changes++);
        var options = layer.ParticipantOptions;
        layer.SyncFromRoute(route, Participants, [], [], []);
        Assert.Same(options, layer.ParticipantOptions);
        Assert.Equal(string.Empty, layer.ParticipantId);
        Assert.Equal(0, changes);
        layer.SetSourceOffset(0.2, 0.1);
        Assert.Equal(SourceRouteMode.ActiveSpeaker, route.Mode);
        Assert.Null(route.ParticipantId);
    }

    [Fact]
    public void MissingSourceIdentityIsRetainedInsteadOfPickingRosterFallback()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.Fixed, ParticipantId = "temporarily-offline" };
        var layer = Model(route);
        layer.SyncFromRoute(route, Participants, [], [], []);
        layer.ApplyRoute();
        Assert.Equal("temporarily-offline", route.ParticipantId);
        Assert.Equal("temporarily-offline", layer.ParticipantId);
    }

    [Fact]
    public void MissingShowInputKeepsItsSlotIdentity()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.Fixed, ShowInputSlotNumber = 4 };
        var layer = Model(route);
        layer.SyncFromRoute(route, Participants, [], [], []);
        Assert.Equal("input-04", layer.ParticipantId);
        Assert.Equal(4, route.ShowInputSlotNumber);
    }
    [Fact]
    public void SourceSelectionIgnoresTransientNullAndSyncCallbacks()
    {
        var route = new SourceRoute { Id = "layer", Mode = SourceRouteMode.Fixed, ParticipantId = "other" };
        var layer = Model(route);
        Assert.False(layer.TrySelectSource(null));
        Assert.False(layer.TrySelectSource("unknown"));
        Assert.Equal("other", route.ParticipantId);
        var automatic = new SourceRoute { Id = "layer", Mode = SourceRouteMode.ActiveSpeaker };
        System.ComponentModel.PropertyChangedEventHandler handler = (_, e) =>
        {
            if (e.PropertyName == nameof(layer.ParticipantOptions))
                Assert.False(layer.TrySelectSource("jamal"));
        };
        layer.PropertyChanged += handler;
        layer.SyncFromRoute(automatic, Participants, [], [], []);
        Assert.Equal(SourceRouteMode.ActiveSpeaker, automatic.Mode);
        Assert.Equal(string.Empty, layer.ParticipantId);
        Assert.Contains(layer.ParticipantOptions, option => option.Value == "" && option.Label == "Automatic - Active Speaker");
        layer.PropertyChanged -= handler;
        Assert.True(layer.TrySelectSource("other"));
        Assert.Equal(SourceRouteMode.Fixed, automatic.Mode);
        Assert.Equal("other", automatic.ParticipantId);
    }
}
