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
        IEnumerable<AudioRoutingCrosspointViewModel> cells)
    {
        SourceId = sourceId;
        SourceLabel = sourceLabel;
        Cells = new ObservableCollection<AudioRoutingCrosspointViewModel>(cells);
    }

    public string SourceId { get; }
    public string SourceLabel { get; }
    public ObservableCollection<AudioRoutingCrosspointViewModel> Cells { get; }
}

/// <summary>
/// Audio routing matrix: sources (rows) × buses (columns), each
/// crosspoint carrying an on/off route plus a gain. Editing happens through the
/// selected crosspoint so the UI stays a single shared gain editor.
/// </summary>
public sealed partial class AudioRoutingMatrixViewModel : ObservableObject
{
    public static IReadOnlyList<RoutingBus> DefaultBuses { get; } =
    [
        new("master", "MASTER"),
        new("pgm-l", "PGM L"),
        new("pgm-r", "PGM R"),
        new("iso-1", "ISO 1"),
        new("iso-2", "ISO 2"),
        new("iso-3", "ISO 3"),
        new("iso-4", "ISO 4"),
        new("iso-5", "ISO 5"),
        new("iso-6", "ISO 6"),
        new("iso-7", "ISO 7"),
        new("iso-8", "ISO 8"),
        new("mon", "MON"),
        new("stream", "STREAM"),
        new("aux-1", "AUX 1"),
        new("aux-2", "AUX 2")
    ];

    // Fresh instances per view-model: RoutingBus now carries mutable state
    // (Label, routed counts, output sends, listen) — sharing the static
    // template objects across VM instances would leak state between them
    // (and between tests). DefaultBuses stays a read-only template.
    public ObservableCollection<RoutingBus> BusHeaders { get; } =
        new(DefaultBuses.Select(bus => new RoutingBus(bus.Id, bus.Label)));

    public ObservableCollection<AudioRoutingSourceRowViewModel> Rows { get; } = [];

    [ObservableProperty]
    private AudioRoutingCrosspointViewModel? _selectedCrosspoint;

    public bool HasRows => Rows.Count > 0;

    public bool HasSelection => SelectedCrosspoint is not null;

    public event Action<AudioRoutingCrosspointViewModel>? RouteChanged;

    private int _customBusCounter = 1;

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
                RouteChanged?.Invoke(SelectedCrosspoint);
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

        // Select never destroys (audio tab redesign B1): clicking a ROUTED
        // cell selects it for gain editing. The old behavior unrouted it, so
        // trying to tweak a level deleted the route instead. Removing a route
        // is the explicit RemoveSelectedRoute command.
        if (cell.IsRouted)
        {
            SelectedCrosspoint = cell;
            return;
        }

        if (IsIsolatedAudioBus(cell.Bus.Id))
        {
            ClearBusColumn(cell.Bus.Id);
        }

        cell.IsRouted = true;
        SelectedCrosspoint = cell;
        RouteChanged?.Invoke(cell);
        RefreshBusRoutedCounts();
    }

    // Lifecycle L3 (source-lifecycle-spec 3.2): clear EVERY send of one source
    // in one act - the per-cell remove made unrouting a source a 5-click chore.
    [RelayCommand]
    private void ClearSourceSends(AudioRoutingSourceRowViewModel row)
    {
        if (row is null)
        {
            return;
        }
        var cleared = 0;
        foreach (var cell in row.Cells)
        {
            if (cell.IsRouted)
            {
                cell.IsRouted = false;
                cleared++;
                RouteChanged?.Invoke(cell);
            }
        }
        if (cleared > 0 && SelectedCrosspoint is not null && row.Cells.Contains(SelectedCrosspoint))
        {
            SelectedCrosspoint = null;
        }
        if (cleared > 0)
        {
            RefreshBusRoutedCounts();
        }
    }

    [RelayCommand]
    private void RemoveSelectedRoute()
    {
        if (SelectedCrosspoint is not { IsRouted: true } cell)
        {
            return;
        }

        cell.IsRouted = false;
        SelectedCrosspoint = null;
        RouteChanged?.Invoke(cell);
        RefreshBusRoutedCounts();
    }

    /// <summary>
    /// Reconciles the grid to the CORE's actual routing state (phase B1): every
    /// listed send becomes a routed cell at its engine gain; cells routed in the
    /// UI but absent from the core are cleared. Sends are keyed by UI source id
    /// (the caller translates engine ids); sends for unknown sources/buses are
    /// ignored. Does NOT raise RouteChanged — this is inbound state, and echoing
    /// it back to the core would ping-pong. No-op for an empty send list (an
    /// idle core hasn't synced yet; clearing the grid then would erase the
    /// operator's staged defaults before the first push).
    /// </summary>
    public bool ApplyCoreSends(IReadOnlyList<(string SourceId, string BusId, double GainDb)> sends)
    {
        if (sends.Count == 0)
        {
            return false;
        }

        var target = sends
            .GroupBy(send => (send.SourceId, send.BusId))
            .ToDictionary(group => group.Key, group => group.Last().GainDb);

        var changed = false;
        foreach (var cell in Rows.SelectMany(row => row.Cells))
        {
            if (target.TryGetValue((cell.SourceId, cell.Bus.Id), out var gainDb))
            {
                if (!cell.IsRouted)
                {
                    cell.IsRouted = true;
                    changed = true;
                }

                if (Math.Abs(cell.GainDb - gainDb) > 0.05)
                {
                    // Skip the selected cell's gain: the operator may be typing
                    // into the gain editor right now.
                    if (!ReferenceEquals(cell, SelectedCrosspoint))
                    {
                        cell.GainDb = gainDb;
                        changed = true;
                    }
                }
            }
            else if (cell.IsRouted)
            {
                cell.IsRouted = false;
                if (ReferenceEquals(cell, SelectedCrosspoint))
                {
                    SelectedCrosspoint = null;
                }

                changed = true;
            }
        }

        if (changed)
        {
            RefreshBusRoutedCounts();
        }

        return changed;
    }

    [RelayCommand]
    private void AddBus()
    {
        var bus = new RoutingBus($"bus-{_customBusCounter:00}", $"BUS {_customBusCounter}");
        _customBusCounter++;
        BusHeaders.Add(bus);

        foreach (var row in Rows)
        {
            row.Cells.Add(new AudioRoutingCrosspointViewModel(row.SourceId, row.SourceLabel, bus));
        }

        OnPropertyChanged(nameof(BusHeaders));
    }

    // Lifecycle L6 (audio-tab-redesign 4, completed): custom/aux buses are
    // deletable; the five fixed program buses and ISO columns are not. Cells
    // for the bus are removed from every row (their sends die with the bus).
    public static bool IsBusRemovable(string busId) =>
        busId.StartsWith("aux-", StringComparison.Ordinal) ||
        busId.StartsWith("bus-", StringComparison.Ordinal);

    // audio-tab-redesign 4: rename an aux/custom bus. The header and every
    // crosspoint cell share the RoutingBus instance, so setting its notifying
    // Label updates the whole column at once. Fixed buses are not renamable.
    public void RenameBus(RoutingBus bus, string newLabel)
    {
        if (bus is null || !IsBusRemovable(bus.Id) || string.IsNullOrWhiteSpace(newLabel))
        {
            return;
        }
        bus.Label = newLabel.Trim();
    }

    [RelayCommand]
    private void RemoveBus(RoutingBus bus)
    {
        if (bus is null || !IsBusRemovable(bus.Id))
        {
            return;
        }
        var removedRoutes = 0;
        foreach (var row in Rows)
        {
            var cell = row.Cells.FirstOrDefault(c => c.Bus.Id == bus.Id);
            if (cell is not null)
            {
                if (cell.IsRouted)
                {
                    cell.IsRouted = false;
                    removedRoutes++;
                    RouteChanged?.Invoke(cell);
                }
                if (ReferenceEquals(SelectedCrosspoint, cell))
                {
                    SelectedCrosspoint = null;
                }
                row.Cells.Remove(cell);
            }
        }
        BusHeaders.Remove(bus);
        OnPropertyChanged(nameof(BusHeaders));
        RefreshBusRoutedCounts();
    }

    /// <summary>
    /// Rebuild the matrix for the given sources, preserving any existing crosspoint
    /// state and defaulting new sources into the program and monitor buses at unity.
    /// </summary>
    public void Build(IReadOnlyList<RoutingSource> sources)
    {
        var previous = Rows
            .SelectMany(row => row.Cells)
            .GroupBy(cell => (cell.SourceId, cell.Bus.Id))
            .ToDictionary(group => group.Key, group =>
            {
                var last = group.Last();
                return (last.IsRouted, last.GainDb);
            });

        Rows.Clear();
        foreach (var source in sources
            .Where(source => !string.IsNullOrWhiteSpace(source.Id))
            .GroupBy(source => source.Id, StringComparer.Ordinal)
            .Select(group => group.Last()))
        {
            var cells = BusHeaders
                .Select(bus =>
                {
                    var cell = new AudioRoutingCrosspointViewModel(source.Id, source.Label, bus);
                    if (previous.TryGetValue((source.Id, bus.Id), out var state))
                    {
                        cell.IsRouted = state.IsRouted;
                        cell.GainDb = state.GainDb;
                    }
                    // Staged defaults for brand-new sources, matching the ENGINE
                    // defaults (EnsureDefault*AudioRoutingSends: master, program,
                    // stream, monitor). "stream" was previously omitted here, so
                    // the grid under-reported what was actually routed until the
                    // core snapshot hydrate (ApplyCoreSends) corrected it.
                    // Z1: sources marked DefaultUnrouted (Zoom ISO) stage no sends -
                    // the operator routes them deliberately.
                    else if (!source.DefaultUnrouted &&
                             bus.Id is "master" or "pgm-l" or "pgm-r" or "stream" or "mon")
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
        RefreshBusRoutedCounts();
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
        foreach (var bus in BusHeaders.Where(bus => IsIsolatedAudioBus(bus.Id)))
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

    // ---- Bus OUTPUT routing (mixer topology) + PFL listen ----------------

    /// <summary>Raised when bus outputs or the listen selection change (sync trigger).</summary>
    public event Action? BusOutputsChanged;

    /// <summary>Flip one of a removable bus's output sends (master/mon/stream).</summary>
    public void SetBusSend(RoutingBus bus, string target, bool enabled)
    {
        if (bus is null || !IsBusRemovable(bus.Id))
        {
            return;
        }

        switch (target)
        {
            case "master": bus.SendsToMaster = enabled; break;
            case "mon": bus.SendsToMonitor = enabled; break;
            case "stream": bus.SendsToStream = enabled; break;
            default: return;
        }

        BusOutputsChanged?.Invoke();
    }

    public void SetBusOutputGainDb(RoutingBus bus, double gainDb)
    {
        if (bus is null || !IsBusRemovable(bus.Id))
        {
            return;
        }

        var clamped = Math.Max(-60, Math.Min(10, gainDb));
        if (Math.Abs(bus.OutputGainDb - clamped) < 0.01)
        {
            return;
        }

        bus.OutputGainDb = clamped;
        BusOutputsChanged?.Invoke();
    }

    /// <summary>PFL: audition one bus on the monitor (exclusive; off = normal MON).</summary>
    public void SetListening(RoutingBus bus, bool listening)
    {
        if (bus is null)
        {
            return;
        }

        foreach (var other in BusHeaders)
        {
            if (!ReferenceEquals(other, bus) && other.IsListening)
            {
                other.IsListening = false;
            }
        }

        bus.IsListening = listening;
        BusOutputsChanged?.Invoke();
    }

    public string? MonitorListenBusId => BusHeaders.FirstOrDefault(bus => bus.IsListening)?.Id;

    /// <summary>The bus→bus sends for the core sync, derived from the card toggles.</summary>
    public IReadOnlyList<(string FromBusId, string ToBusId, double GainDb)> BuildBusSends()
    {
        var sends = new List<(string, string, double)>();
        foreach (var bus in BusHeaders)
        {
            if (!IsBusRemovable(bus.Id))
            {
                continue;
            }
            if (bus.SendsToMaster)
            {
                sends.Add((bus.Id, "master", bus.OutputGainDb));
            }
            if (bus.SendsToMonitor)
            {
                sends.Add((bus.Id, "mon", bus.OutputGainDb));
            }
            if (bus.SendsToStream)
            {
                sends.Add((bus.Id, "stream", bus.OutputGainDb));
            }
        }

        return sends;
    }

    /// <summary>
    /// U2: keep each bus's RoutedSourceCount fresh so the bus cards can say
    /// "N sources routed" without their own matrix scan. Called after every
    /// route mutation (toggle/clear/remove/apply/build). Cheap: rows × buses.
    /// RoutingBus.RoutedSourceCount only notifies on an actual value change.
    /// </summary>
    public void RefreshBusRoutedCounts()
    {
        foreach (var bus in BusHeaders)
        {
            var count = 0;
            foreach (var row in Rows)
            {
                foreach (var cell in row.Cells)
                {
                    if (cell.IsRouted && ReferenceEquals(cell.Bus, bus))
                    {
                        count++;
                    }
                }
            }

            bus.RoutedSourceCount = count;
        }
    }
}
