using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class RoutingMatrixViewModelTests
{
    [Fact]
    public void VideoRouting_IsoDestinationAllowsOnlyOneSource()
    {
        var viewModel = new VideoRoutingMatrixViewModel();
        viewModel.Build(
            [
                new RoutingSource("camera-1", "Camera 1"),
                new RoutingSource("camera-2", "Camera 2")
            ]);

        var firstIso = FindVideoCell(viewModel, "camera-1", "iso-1");
        var secondIso = FindVideoCell(viewModel, "camera-2", "iso-1");

        viewModel.SelectCrosspointCommand.Execute(firstIso);
        viewModel.SelectCrosspointCommand.Execute(secondIso);

        Assert.False(firstIso.IsRouted);
        Assert.True(secondIso.IsRouted);
    }

    [Fact]
    public void VideoRouting_NonIsoDestinationAllowsMultipleSourcesAndClickRemoves()
    {
        var viewModel = new VideoRoutingMatrixViewModel();
        viewModel.Build(
            [
                new RoutingSource("camera-1", "Camera 1"),
                new RoutingSource("camera-2", "Camera 2")
            ]);

        var firstMv = FindVideoCell(viewModel, "camera-1", "multiview");
        var secondMv = FindVideoCell(viewModel, "camera-2", "multiview");

        viewModel.SelectCrosspointCommand.Execute(firstMv);
        viewModel.SelectCrosspointCommand.Execute(secondMv);

        Assert.True(firstMv.IsRouted);
        Assert.True(secondMv.IsRouted);

        viewModel.SelectCrosspointCommand.Execute(firstMv);
        Assert.False(firstMv.IsRouted);
        Assert.True(secondMv.IsRouted);
    }

    [Fact]
    public void AudioRouting_IsoBusAllowsOnlyOneSourceAndRemoveSelectedClearsIt()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build(
            [
                new RoutingSource("mic-1", "Mic 1"),
                new RoutingSource("mic-2", "Mic 2")
            ]);

        var firstIso = FindAudioCell(viewModel, "mic-1", "iso-1");
        var secondIso = FindAudioCell(viewModel, "mic-2", "iso-1");

        viewModel.SelectCrosspointCommand.Execute(firstIso);
        viewModel.SelectCrosspointCommand.Execute(secondIso);

        Assert.False(firstIso.IsRouted);
        Assert.True(secondIso.IsRouted);
        Assert.Same(secondIso, viewModel.SelectedCrosspoint);

        viewModel.RemoveSelectedCommand.Execute(null);

        Assert.False(secondIso.IsRouted);
        Assert.Same(secondIso, viewModel.SelectedCrosspoint);
    }

    [Fact]
    public void AudioRouting_AddBusExtendsExistingRows()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build(
            [
                new RoutingSource("mic-1", "Mic 1")
            ]);

        viewModel.AddBusCommand.Execute(null);

        Assert.Contains(viewModel.BusHeaders, bus => bus.Id == "bus-01");
        Assert.NotNull(FindAudioCell(viewModel, "mic-1", "bus-01"));
    }

    private static VideoRoutingCrosspointViewModel FindVideoCell(
        VideoRoutingMatrixViewModel viewModel,
        string sourceId,
        string destinationId) =>
        viewModel.Rows
            .Single(row => row.SourceId == sourceId)
            .Cells
            .Single(cell => cell.Destination.Id == destinationId);

    private static AudioRoutingCrosspointViewModel FindAudioCell(
        AudioRoutingMatrixViewModel viewModel,
        string sourceId,
        string busId) =>
        viewModel.Rows
            .Single(row => row.SourceId == sourceId)
            .Cells
            .Single(cell => cell.Bus.Id == busId);
}
