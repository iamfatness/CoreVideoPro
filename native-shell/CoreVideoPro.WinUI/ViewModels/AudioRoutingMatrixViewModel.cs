using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// One crosspoint in the audio routing matrix: a single source → bus send with an
/// on/off state and a gain in dB (the "full gain matrix" interaction model).
/// </summary>
public sealed partial class AudioRoutingCrosspointViewModel : ObservableObject
{
    public AudioRoutingCrosspointViewModel(string sourceId, string sourceLabel, RoutingBus bus)
    {
        SourceId = sourceId;
        SourceLabel = sourceLabel;
        Bus = bus;
    }

    public string SourceId { get; }
    public string SourceLabel { get; }
    public RoutingBus Bus { get; }
    public string BusLabel => Bus.Label;

    [ObservableProperty]
    private bool _isRouted;

    [ObservableProperty]
    private double _gainDb;

    public string GainLabel => IsRouted ? GainDb.ToString("0.0") : "·";

    public Brush CellBackground => new SolidColorBrush(IsRouted
        ? Color.FromArgb(64, 68, 193, 161)
        : Color.FromArgb(255, 12, 17, 24));

    public Brush CellBorderBrush => new SolidColorBrush(IsRouted
        ? Color.FromArgb(180, 68, 193, 161)
        : Color.FromArgb(60, 255, 255, 255));

    public Brush CellForeground => new SolidColorBrush(IsRouted
        ? Color.FromArgb(255, 224, 244, 236)
        : Color.FromArgb(120, 140, 154, 148));

    partial void OnIsRoutedChanged(bool value)
    {
        OnPropertyChanged(nameof(GainLabel));
        OnPropertyChanged(nameof(CellBackground));
        OnPropertyChanged(nameof(CellBorderBrush));
        OnPropertyChanged(nameof(CellForeground));
    }

    partial void OnGainDbChanged(double value) => OnPropertyChanged(nameof(GainLabel));
}

/// <summary>One matrix row: a source and its crosspoint cell for every bus.</summary>
public sealed class AudioRoutingSourceRowViewModel
{
    public AudioRoutingSourceRowViewModel(
        string sourceId,
        string sourceLabel,
        IReadOnlyList<AudioRoutingCrosspointViewModel> cells)
    {
        SourceId = sourceId;
        SourceLabel = sourceLabel;
        Cells = cells;
    }

    public string SourceId { get; }
    public string SourceLabel { get; }
    public IReadOnlyList<AudioRoutingCrosspointViewModel> Cells { get; }
}

/// <summary>
/// Dante-style audio routing matrix: sources (rows) × buses (columns), each
/// crosspoint carrying an on/off route plus a gain. Editing happens through the
/// selected crosspoint so the UI stays a single shared gain editor.
/// </summary>
public sealed partial class AudioRoutingMatrixViewModel : ObservableObject
{
    public static IReadOnlyList<RoutingBus> Buses { get; } =
    [
        new("pgm-l", "PGM L"),
        new("pgm-r", "PGM R"),
        new("iso-1", "ISO 1"),
        new("iso-2", "ISO 2"),
        new("mon", "MON"),
        new("stream", "STREAM")
    ];

    public IReadOnlyList<RoutingBus> BusHeaders => Buses;

    public ObservableCollection<AudioRoutingSourceRowViewModel> Rows { get; } = [];

    [ObservableProperty]
    private AudioRoutingCrosspointViewModel? _selectedCrosspoint;

    public bool HasRows => Rows.Count > 0;

    public bool HasSelection => SelectedCrosspoint is not null;

    public string SelectionSummary => SelectedCrosspoint is null
        ? "Select a crosspoint to route it and set its level."
        : $"{SelectedCrosspoint.SourceLabel}  →  {SelectedCrosspoint.BusLabel}";

    public double SelectedGainDb
    {
        get => SelectedCrosspoint?.GainDb ?? 0;
        set
        {
            if (SelectedCrosspoint is not null)
            {
                SelectedCrosspoint.GainDb = value;
            }

            OnPropertyChanged();
        }
    }

    partial void OnSelectedCrosspointChanged(AudioRoutingCrosspointViewModel? value)
    {
        OnPropertyChanged(nameof(HasSelection));
        OnPropertyChanged(nameof(SelectionSummary));
        OnPropertyChanged(nameof(SelectedGainDb));
    }

    [RelayCommand]
    private void SelectCrosspoint(AudioRoutingCrosspointViewModel? cell)
    {
        if (cell is null)
        {
            return;
        }

        if (!cell.IsRouted)
        {
            if (IsIsolatedAudioBus(cell.Bus.Id))
            {
                ClearBusColumn(cell.Bus.Id);
            }

            cell.IsRouted = true;
        }

        SelectedCrosspoint = cell;
    }

    [RelayCommand]
    private void RemoveSelected()
    {
        if (SelectedCrosspoint is null)
        {
            return;
        }

        SelectedCrosspoint.IsRouted = false;
        OnPropertyChanged(nameof(SelectedGainDb));
    }

    /// <summary>
    /// Rebuild the matrix for the given sources, preserving any existing crosspoint
    /// state and defaulting new sources into the program (PGM L/R) buses at unity.
    /// </summary>
    public void Build(IReadOnlyList<RoutingSource> sources)
    {
        var previous = Rows
            .SelectMany(row => row.Cells)
            .ToDictionary(cell => (cell.SourceId, cell.Bus.Id), cell => (cell.IsRouted, cell.GainDb));

        Rows.Clear();
        foreach (var source in sources)
        {
            var cells = Buses
                .Select(bus =>
                {
                    var cell = new AudioRoutingCrosspointViewModel(source.Id, source.Label, bus);
                    if (previous.TryGetValue((source.Id, bus.Id), out var state))
                    {
                        cell.IsRouted = state.IsRouted;
                        cell.GainDb = state.GainDb;
                    }
                    else if (bus.Id is "pgm-l" or "pgm-r")
                    {
                        cell.IsRouted = true;
                        cell.GainDb = 0;
                    }

                    return cell;
                })
                .ToList();

            Rows.Add(new AudioRoutingSourceRowViewModel(source.Id, source.Label, cells));
        }

        NormalizeIsolatedAudioBuses();
        SelectedCrosspoint = null;
        OnPropertyChanged(nameof(HasRows));
    }

    private void ClearBusColumn(string busId)
    {
        foreach (var existing in Rows
            .SelectMany(row => row.Cells)
            .Where(existing => existing.Bus.Id == busId))
        {
            existing.IsRouted = false;
        }
    }

    private void NormalizeIsolatedAudioBuses()
    {
        foreach (var bus in Buses.Where(bus => IsIsolatedAudioBus(bus.Id)))
        {
            var routedCells = Rows
                .SelectMany(row => row.Cells)
                .Where(cell => cell.Bus.Id == bus.Id && cell.IsRouted)
                .ToList();

            foreach (var duplicate in routedCells.Skip(1))
            {
                duplicate.IsRouted = false;
            }
        }
    }

    private static bool IsIsolatedAudioBus(string busId) =>
        busId.StartsWith("iso-", StringComparison.OrdinalIgnoreCase);
}
