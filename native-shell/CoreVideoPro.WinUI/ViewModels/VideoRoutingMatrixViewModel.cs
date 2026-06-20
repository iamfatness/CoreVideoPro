using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// One crosspoint in the video routing matrix: a single source → destination route
/// with an on/off state only (no gain). The video matrix governs ISO recording,
/// multiview tiles, and aux/stream sends; clicking a cell toggles its route.
/// </summary>
public sealed partial class VideoRoutingCrosspointViewModel : ObservableObject
{
    public VideoRoutingCrosspointViewModel(string sourceId, string sourceLabel, RoutingDestination destination)
    {
        SourceId = sourceId;
        SourceLabel = sourceLabel;
        Destination = destination;
    }

    public string SourceId { get; }
    public string SourceLabel { get; }
    public RoutingDestination Destination { get; }
    public string DestinationLabel => Destination.Label;

    [ObservableProperty]
    private bool _isRouted;

    public string RouteLabel => IsRouted ? "●" : "·";

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
        OnPropertyChanged(nameof(RouteLabel));
        OnPropertyChanged(nameof(CellBackground));
        OnPropertyChanged(nameof(CellBorderBrush));
        OnPropertyChanged(nameof(CellForeground));
    }
}

/// <summary>One matrix row: a source and its crosspoint cell for every destination.</summary>
public sealed class VideoRoutingSourceRowViewModel
{
    public VideoRoutingSourceRowViewModel(
        string sourceId,
        string sourceLabel,
        IReadOnlyList<VideoRoutingCrosspointViewModel> cells)
    {
        SourceId = sourceId;
        SourceLabel = sourceLabel;
        Cells = cells;
    }

    public string SourceId { get; }
    public string SourceLabel { get; }
    public IReadOnlyList<VideoRoutingCrosspointViewModel> Cells { get; }
}

/// <summary>
/// Video routing matrix: sources (rows) × destinations (columns), each
/// crosspoint carrying an on/off route only (no gain). Clicking a cell toggles its
/// route directly — there is no shared editor, unlike the audio gain matrix.
/// </summary>
public sealed partial class VideoRoutingMatrixViewModel : ObservableObject
{
    public static IReadOnlyList<RoutingDestination> Destinations { get; } =
    [
        new("iso-1", "ISO 1"),
        new("iso-2", "ISO 2"),
        new("iso-3", "ISO 3"),
        new("iso-4", "ISO 4"),
        new("iso-5", "ISO 5"),
        new("iso-6", "ISO 6"),
        new("iso-7", "ISO 7"),
        new("iso-8", "ISO 8"),
        new("multiview", "MULTIVIEW"),
        new("aux", "AUX")
    ];

    public IReadOnlyList<RoutingDestination> DestinationHeaders => Destinations;

    public ObservableCollection<VideoRoutingSourceRowViewModel> Rows { get; } = [];

    public bool HasRows => Rows.Count > 0;

    public event Action<VideoRoutingCrosspointViewModel>? RouteChanged;

    [RelayCommand]
    private void SelectCrosspoint(VideoRoutingCrosspointViewModel? cell)
    {
        if (cell is null)
        {
            return;
        }

        if (cell.IsRouted)
        {
            cell.IsRouted = false;
            RouteChanged?.Invoke(cell);
            return;
        }

        if (IsExclusiveDestination(cell.Destination.Id))
        {
            ClearDestinationColumn(cell.Destination.Id);
        }

        cell.IsRouted = true;
        RouteChanged?.Invoke(cell);
    }

    /// <summary>
    /// Rebuild the matrix for the given sources, preserving any existing crosspoint
    /// state. New sources start with every destination unrouted.
    /// </summary>
    public void Build(IReadOnlyList<RoutingSource> sources)
    {
        var previous = Rows
            .SelectMany(row => row.Cells)
            .ToDictionary(cell => (cell.SourceId, cell.Destination.Id), cell => cell.IsRouted);

        Rows.Clear();
        foreach (var source in sources)
        {
            var cells = Destinations
                .Select(destination =>
                {
                    var cell = new VideoRoutingCrosspointViewModel(source.Id, source.Label, destination);
                    if (previous.TryGetValue((source.Id, destination.Id), out var isRouted))
                    {
                        cell.IsRouted = isRouted;
                    }

                    return cell;
                })
                .ToList();

            Rows.Add(new VideoRoutingSourceRowViewModel(source.Id, source.Label, cells));
        }

        NormalizeExclusiveDestinations();
        OnPropertyChanged(nameof(HasRows));
    }

    public void SetRoute(string sourceId, string destinationId, bool isRouted)
    {
        var cell = Rows
            .SelectMany(row => row.Cells)
            .FirstOrDefault(cell =>
                string.Equals(cell.SourceId, sourceId, StringComparison.Ordinal) &&
                string.Equals(cell.Destination.Id, destinationId, StringComparison.Ordinal));
        if (cell is not null)
        {
            cell.IsRouted = isRouted;
        }
    }

    private void ClearDestinationColumn(string destinationId)
    {
        foreach (var existing in Rows
            .SelectMany(row => row.Cells)
            .Where(existing => existing.Destination.Id == destinationId))
        {
            existing.IsRouted = false;
        }
    }

    private void NormalizeExclusiveDestinations()
    {
        foreach (var destination in Destinations.Where(destination => IsExclusiveDestination(destination.Id)))
        {
            var routedCells = Rows
                .SelectMany(row => row.Cells)
                .Where(cell => cell.Destination.Id == destination.Id && cell.IsRouted)
                .ToList();

            foreach (var duplicate in routedCells.Skip(1))
            {
                duplicate.IsRouted = false;
            }
        }
    }

    private static bool IsExclusiveDestination(string destinationId) =>
        destinationId.StartsWith("iso-", StringComparison.OrdinalIgnoreCase);
}
