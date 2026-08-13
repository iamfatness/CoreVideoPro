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
    public void AudioRouting_IsoBusAllowsOnlyOneSourceAndTogglingClearsIt()
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

        // B1: select never destroys — re-selecting a routed crosspoint keeps the
        // route (it used to delete it). Removal is the explicit command.
        viewModel.SelectCrosspointCommand.Execute(secondIso);

        Assert.True(secondIso.IsRouted);
        Assert.Same(secondIso, viewModel.SelectedCrosspoint);

        viewModel.RemoveSelectedRouteCommand.Execute(null);

        Assert.False(secondIso.IsRouted);
        Assert.Null(viewModel.SelectedCrosspoint);
    }

    [Fact]
    public void AudioRouting_ApplyCoreSendsReconcilesRoutesAndGainsWithoutEvents()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build([new RoutingSource("mic-1", "Mic 1")]);

        var routeChangedCount = 0;
        viewModel.RouteChanged += _ => routeChangedCount++;

        // Core says: master at -6, iso-1 at 0. Everything else (the staged
        // defaults incl. pgm-l/pgm-r/stream/mon) must clear to match reality.
        var changed = viewModel.ApplyCoreSends(
            [("mic-1", "master", -6.0), ("mic-1", "iso-1", 0.0)]);

        Assert.True(changed);
        Assert.Equal(0, routeChangedCount);  // inbound state never echoes back
        var master = FindAudioCell(viewModel, "mic-1", "master");
        Assert.True(master.IsRouted);
        Assert.Equal(-6.0, master.GainDb, 1);
        Assert.True(FindAudioCell(viewModel, "mic-1", "iso-1").IsRouted);
        Assert.False(FindAudioCell(viewModel, "mic-1", "pgm-l").IsRouted);
        Assert.False(FindAudioCell(viewModel, "mic-1", "stream").IsRouted);
        Assert.False(FindAudioCell(viewModel, "mic-1", "mon").IsRouted);

        // Unknown sources/buses are ignored; identical state reports no change.
        Assert.False(viewModel.ApplyCoreSends(
            [("mic-1", "master", -6.0), ("mic-1", "iso-1", 0.0), ("ghost", "master", 0.0), ("mic-1", "no-such-bus", 0.0)]));
    }

    [Fact]
    public void AudioRouting_ApplyCoreSendsWithEmptyListIsANoOp()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build([new RoutingSource("mic-1", "Mic 1")]);

        // An idle core hasn't synced yet — an empty send list must not wipe the
        // staged defaults before the first push.
        Assert.False(viewModel.ApplyCoreSends([]));
        Assert.True(FindAudioCell(viewModel, "mic-1", "master").IsRouted);
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

    [Fact]
    public void AudioRouting_NewSourcesDefaultToProgramNotMonitor()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build(
            [
                new RoutingSource("local-machine-audio", "Local machine audio")
            ]);

        Assert.True(FindAudioCell(viewModel, "local-machine-audio", "master").IsRouted);
        Assert.True(FindAudioCell(viewModel, "local-machine-audio", "pgm-l").IsRouted);
        Assert.True(FindAudioCell(viewModel, "local-machine-audio", "pgm-r").IsRouted);
        // "stream" matches the engine defaults (EnsureDefault*AudioRoutingSends);
        // it was previously omitted, so the grid under-reported real routing.
        Assert.True(FindAudioCell(viewModel, "local-machine-audio", "stream").IsRouted);
        // "mon" is never staged by default (live meeting 2026-08-09): the
        // monitor follows MASTER unless the operator explicitly routes the MON
        // column — a default mon crosspoint captured the monitor away from the
        // program the operator was producing.
        Assert.False(FindAudioCell(viewModel, "local-machine-audio", "mon").IsRouted);
    }

    [Fact]
    public void AudioRouting_DuplicateSourcesDoNotCreateDuplicateRows()
    {
        var viewModel = new AudioRoutingMatrixViewModel();

        viewModel.Build(
            [
                new RoutingSource("local-machine-audio", "Local machine audio"),
                new RoutingSource("local-machine-audio", "Local machine audio duplicate"),
                new RoutingSource("capture:uvc-01", "UVC audio"),
                new RoutingSource("capture:uvc-01", "UVC audio duplicate")
            ]);

        Assert.Equal(2, viewModel.Rows.Count);
        Assert.Single(viewModel.Rows, row => row.SourceId == "local-machine-audio");
        Assert.Single(viewModel.Rows, row => row.SourceId == "capture:uvc-01");
    }

    [Fact]
    public void AudioRouting_RebuildWithDuplicateExistingCellsDoesNotThrow()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build([new RoutingSource("mic-1", "Mic 1")]);
        viewModel.Rows[0].Cells.Add(new AudioRoutingCrosspointViewModel("mic-1", "Mic 1", viewModel.BusHeaders[0])
        {
            IsRouted = true,
            GainDb = -6
        });

        viewModel.Build([new RoutingSource("mic-1", "Mic 1")]);

        Assert.Single(viewModel.Rows);
        Assert.NotNull(FindAudioCell(viewModel, "mic-1", "master"));
    }

    // ---- U2: bus cards — purpose text + live routed counts ----

    [Fact]
    public void BusPurpose_EveryDefaultBusExplainsItselfInWords()
    {
        foreach (var bus in AudioRoutingMatrixViewModel.DefaultBuses)
        {
            Assert.False(string.IsNullOrWhiteSpace(bus.Purpose), $"bus '{bus.Id}' has no purpose text");
            Assert.DoesNotContain("processing", bus.Id);  // ids never leak into purpose copy
        }

        Assert.Contains("audience", new RoutingBus("master", "MASTER").Purpose);
        Assert.Contains("headphones", new RoutingBus("mon", "MON").Purpose);
        Assert.Contains("stream", new RoutingBus("stream", "STREAM").Purpose, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("ONE source", new RoutingBus("iso-3", "ISO 3").Purpose);
        Assert.Contains("custom mix", new RoutingBus("aux-1", "AUX 1").Purpose);
    }

    [Fact]
    public void BusRoutedCounts_TrackBuildToggleAndRemove()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build(
            [
                new RoutingSource("mic-1", "Mic 1"),
                new RoutingSource("mic-2", "Mic 2")
            ]);

        var master = viewModel.BusHeaders.Single(bus => bus.Id == "master");
        var aux = viewModel.BusHeaders.Single(bus => bus.Id == "aux-1");

        // Build stages both new sources onto the program defaults.
        Assert.Equal(2, master.RoutedSourceCount);
        Assert.Equal(0, aux.RoutedSourceCount);
        Assert.Equal("Nothing routed", aux.RoutedSummary);

        // Routing a cell updates the count live.
        viewModel.SelectCrosspointCommand.Execute(FindAudioCell(viewModel, "mic-1", "aux-1"));
        Assert.Equal(1, aux.RoutedSourceCount);
        Assert.Equal("1 source routed", aux.RoutedSummary);

        // Removing the selected route updates it back down.
        viewModel.RemoveSelectedRouteCommand.Execute(null);
        Assert.Equal(0, aux.RoutedSourceCount);

        // Clearing a whole source row drops every bus it fed.
        viewModel.ClearSourceSendsCommand.Execute(viewModel.Rows.Single(row => row.SourceId == "mic-1"));
        Assert.Equal(1, master.RoutedSourceCount);
    }

    [Fact]
    public void BusOutputs_BuildBusSendsReflectsTheCardToggles()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        var aux = viewModel.BusHeaders.Single(bus => bus.Id == "aux-1");
        var master = viewModel.BusHeaders.Single(bus => bus.Id == "master");

        var changes = 0;
        viewModel.BusOutputsChanged += () => changes++;

        viewModel.SetBusSend(aux, "master", true);
        viewModel.SetBusSend(aux, "mon", true);
        viewModel.SetBusOutputGainDb(aux, -6);
        viewModel.SetBusSend(master, "mon", true);  // fixed buses have no outputs - ignored

        var sends = viewModel.BuildBusSends();
        Assert.Equal(2, sends.Count);
        Assert.Contains(("aux-1", "master", -6.0), sends);
        Assert.Contains(("aux-1", "mon", -6.0), sends);
        Assert.Equal(3, changes);  // two toggles + gain; the fixed-bus call did not fire

        viewModel.SetBusSend(aux, "master", false);
        Assert.Single(viewModel.BuildBusSends());
    }

    [Fact]
    public void BusListen_IsExclusiveAndExposesTheListenBusId()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        var aux1 = viewModel.BusHeaders.Single(bus => bus.Id == "aux-1");
        var aux2 = viewModel.BusHeaders.Single(bus => bus.Id == "aux-2");

        Assert.Null(viewModel.MonitorListenBusId);

        viewModel.SetListening(aux1, true);
        Assert.Equal("aux-1", viewModel.MonitorListenBusId);

        viewModel.SetListening(aux2, true);  // exclusive: aux-1 drops
        Assert.False(aux1.IsListening);
        Assert.Equal("aux-2", viewModel.MonitorListenBusId);

        viewModel.SetListening(aux2, false);
        Assert.Null(viewModel.MonitorListenBusId);
    }

    [Fact]
    public void BusRoutedCounts_ApplyCoreSendsRefreshes()
    {
        var viewModel = new AudioRoutingMatrixViewModel();
        viewModel.Build([new RoutingSource("mic-1", "Mic 1", DefaultUnrouted: true)]);

        var master = viewModel.BusHeaders.Single(bus => bus.Id == "master");
        Assert.Equal(0, master.RoutedSourceCount);

        viewModel.ApplyCoreSends([("mic-1", "master", 0.0)]);
        Assert.Equal(1, master.RoutedSourceCount);
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
