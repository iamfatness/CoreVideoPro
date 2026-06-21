using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Covers the §4 IA cleanup for the Scenes tab: the per-layer Route(Mode)/AudioRole
/// operator controls were removed so the Routing tab matrices are the single source of
/// truth, while an explicit "Add source" affordance lists the Inputs.
/// </summary>
public sealed class SceneCanvasIaTests
{
    [Fact]
    public void AddSourceOptions_ListsInputsOneThroughTenPlusAutoRoles()
    {
        var options = SceneRoutingService.AddSourceOptions;

        for (var slot = 1; slot <= ShowInputRosterService.MaxShowInputs; slot++)
        {
            var value = $"input-{slot:00}";
            Assert.Contains(options, option => option.Value == value && option.Label == $"Input {slot}");
        }

        Assert.Contains(options, option => option.Value == "active-speaker");
        Assert.Contains(options, option => option.Value == "screen-share");
        Assert.Contains(options, option => option.Value == "media");

        // Inputs 1-10 plus the three auto roles.
        Assert.Equal(ShowInputRosterService.MaxShowInputs + 3, options.Count);
    }

    [Fact]
    public void GetRouteDefaults_PreservesOperatorAddedSourcesBeyondLayoutSlotCount()
    {
        // "host-focus" maps to a single-slot layout, but an operator can append sources via
        // the "Add source" affordance; reconciliation must not truncate them away.
        var scene = new Scene { Id = "scene-1", Name = "Solo", Layout = "host-focus" };
        var participants = new List<Participant>
        {
            new() { Id = "p1", Name = "Host" }
        };

        var existing = new List<SourceRoute>
        {
            new() { Id = "scene-1-1", Mode = SourceRouteMode.Fixed, ParticipantId = "p1" },
            new() { Id = "scene-1-2", Mode = SourceRouteMode.Fixed, ShowInputSlotNumber = 2 },
            new() { Id = "scene-1-3", Mode = SourceRouteMode.ScreenShare }
        };

        var defaults = SceneRoutingService.GetRouteDefaults(scene, existing, participants);

        Assert.Equal(existing.Count, defaults.Count);
    }

    [Fact]
    public void Layer_SourceChange_DoesNotIndependentlyDriveAudioRole()
    {
        // The per-layer AudioRole dropdown is gone: changing the source picker must keep the
        // route's audio role at its initialized default (audio is matrix-governed).
        var route = new SourceRoute
        {
            Id = "scene-1-1",
            Mode = SourceRouteMode.Fixed,
            ParticipantId = "p1",
            AudioRole = SourceAudioRole.Mix
        };

        var participants = new List<Participant>
        {
            new() { Id = "p1", Name = "Host" },
            new() { Id = "p2", Name = "Guest" }
        };

        var changes = 0;
        var layer = new SceneCanvasLayerViewModel(
            0,
            route,
            participants,
            captureDevices: [],
            showInputs: [],
            _ => changes++);

        layer.ParticipantId = "p2";

        Assert.True(changes > 0);
        Assert.Equal(SourceAudioRole.Mix, route.AudioRole);
    }

    [Fact]
    public void IsoAudioAuthority_IsTheVideoRoutingMatrix_NotPerLayerAudioRole()
    {
        // BuildIsoParticipantTargets prefers iso- matrix crosspoints. Confirm the matrix can
        // independently designate an iso destination (the single audio source of truth).
        var matrix = new VideoRoutingMatrixViewModel();
        matrix.Build(
            [
                new RoutingSource("input-01", "Input 1"),
                new RoutingSource("input-02", "Input 2")
            ]);

        var iso = matrix.Rows
            .Single(row => row.SourceId == "input-01")
            .Cells
            .Single(cell => cell.Destination.Id == "iso-1");

        matrix.SelectCrosspointCommand.Execute(iso);

        Assert.True(iso.IsRouted);
    }
}
