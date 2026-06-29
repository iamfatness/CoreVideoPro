using System;
using System.Collections;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.Windows.Input;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class ShowMultiviewHost : UserControl
{
    public static readonly DependencyProperty TilesProperty =
        DependencyProperty.Register(nameof(Tiles), typeof(IEnumerable), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnTilesChanged));

    public static readonly DependencyProperty TileClickCommandProperty =
        DependencyProperty.Register(nameof(TileClickCommand), typeof(ICommand), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnTileClickCommandChanged));

    private bool _rebuildInProgress;
    private bool _rebuildScheduled;
    private IReadOnlyList<ParticipantSurfaceTile> _lastBuiltTiles = [];
    private (int Columns, int Rows) _currentShape = (0, 0);

    public ShowMultiviewHost()
    {
        InitializeComponent();
        Loaded += (_, _) => ScheduleRebuildLayout();
        LayoutSurface.SizeChanged += OnLayoutSizeChanged;
    }

    // The optimal grid shape depends on the container's aspect ratio, so re-lay-out when a
    // resize changes the chosen rows/cols (e.g. dragging the program/multiview splitter).
    // Tiles are CPU surfaces here, so this rebuild has no GPU swap-chain churn.
    private void OnLayoutSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_lastBuiltTiles.Count == 0 || _rebuildInProgress)
        {
            return;
        }

        if (ResolveGridShape(_lastBuiltTiles.Count) == _currentShape)
        {
            return;
        }

        _rebuildInProgress = true;
        try
        {
            RebuildLayoutCore(_lastBuiltTiles);
        }
        finally
        {
            _rebuildInProgress = false;
        }
    }

    public IEnumerable? Tiles
    {
        get => (IEnumerable?)GetValue(TilesProperty);
        set => SetValue(TilesProperty, value);
    }

    public ICommand? TileClickCommand
    {
        get => (ICommand?)GetValue(TileClickCommandProperty);
        set => SetValue(TileClickCommandProperty, value);
    }

    private static void OnTilesChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host.ScheduleRebuildLayout();
        }
    }

    private static void OnTileClickCommandChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host.ApplyTileClickCommandToChildren();
        }
    }

    private void ScheduleRebuildLayout()
    {
        if (_rebuildScheduled || _rebuildInProgress)
        {
            return;
        }

        _rebuildScheduled = true;
        _ = DispatcherQueue.TryEnqueue(() =>
        {
            _rebuildScheduled = false;
            RebuildLayout();
        });
    }

    private void RebuildLayout()
    {
        if (_rebuildInProgress)
        {
            return;
        }

        _rebuildInProgress = true;
        try
        {
            var tiles = ShowInputRosterService.SelectVisibleMultiviewTiles(Tiles);
            if (TryPatchTileSurfaces(tiles))
            {
                return;
            }

            RebuildLayoutCore(tiles);
            _lastBuiltTiles = tiles;
        }
        finally
        {
            _rebuildInProgress = false;
        }
    }

    private bool TryPatchTileSurfaces(IReadOnlyList<ParticipantSurfaceTile> tiles)
    {
        if (_lastBuiltTiles.Count == 0 ||
            !ShowInputRosterService.SameMultiviewTileStructure(_lastBuiltTiles, tiles))
        {
            return false;
        }

        var patchIndex = 0;
        foreach (var child in LayoutSurface.Children)
        {
            var multiviewTile = child switch
            {
                BroadcastMultiviewTile direct => direct,
                AspectRatioHost { Child: BroadcastMultiviewTile nested } => nested,
                _ => null
            };

            if (multiviewTile is null)
            {
                continue;
            }

            if (patchIndex >= tiles.Count)
            {
                return false;
            }

            multiviewTile.Tile = tiles[patchIndex];
            multiviewTile.TileClickCommand = TileClickCommand;
            patchIndex++;
        }

        if (patchIndex != tiles.Count)
        {
            return false;
        }

        _lastBuiltTiles = tiles;
        return true;
    }

    private void RebuildLayoutCore(IReadOnlyList<ParticipantSurfaceTile> tiles)
    {
        LayoutSurface.Children.Clear();
        LayoutSurface.RowDefinitions.Clear();
        LayoutSurface.ColumnDefinitions.Clear();

        EmptyState.Visibility = tiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (tiles.Count == 0)
        {
            Grid.SetRow(EmptyState, 0);
            Grid.SetColumn(EmptyState, 0);
            LayoutSurface.Children.Add(EmptyState);
            return;
        }

        var (columns, rows) = ResolveGridShape(tiles.Count);
        _currentShape = (columns, rows);
        for (var column = 0; column < columns; column++)
        {
            LayoutSurface.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }

        for (var row = 0; row < rows; row++)
        {
            LayoutSurface.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        }

        for (var index = 0; index < tiles.Count; index++)
        {
            var tile = tiles[index];
            var row = index / columns;
            var column = index % columns;
            AddTile(tile, row, column);
        }
    }

    // Choose the rows×cols that maximizes each (16:9) tile's area inside the actual
    // container, so the grid fills the space instead of letterboxing every tile. A fixed
    // count→shape table wastes space because it ignores the container aspect ratio (the
    // multiview pane is wide and resizable). Falls back to a count table before first measure.
    private (int Columns, int Rows) ResolveGridShape(int tileCount)
    {
        if (tileCount <= 1)
        {
            return (1, 1);
        }

        var width = LayoutSurface.ActualWidth;
        var height = LayoutSurface.ActualHeight;
        if (width <= 0 || height <= 0)
        {
            return tileCount switch
            {
                2 => (2, 1),
                3 => (3, 1),
                <= 4 => (2, 2),
                <= 6 => (3, 2),
                <= 8 => (4, 2),
                <= 10 => (5, 2),
                <= 12 => (4, 3),
                _ => (4, 4)
            };
        }

        const double tileAspect = 16.0 / 9.0;
        var bestColumns = 1;
        var bestRows = tileCount;
        var bestArea = 0.0;
        for (var columns = 1; columns <= tileCount; columns++)
        {
            var rows = (int)Math.Ceiling((double)tileCount / columns);
            var cellWidth = width / columns;
            var cellHeight = height / rows;
            // Largest 16:9 tile that fits the cell (letterboxed within the cell).
            var tileWidth = Math.Min(cellWidth, cellHeight * tileAspect);
            var tileHeight = tileWidth / tileAspect;
            var area = tileWidth * tileHeight;
            if (area > bestArea + 0.5)
            {
                bestArea = area;
                bestColumns = columns;
                bestRows = rows;
            }
        }

        return (bestColumns, bestRows);
    }

    private void AddTile(ParticipantSurfaceTile tile, int row, int column, int rowSpan = 1, int columnSpan = 1)
    {
        var host = new BroadcastMultiviewTile
        {
            Tile = tile,
            TileClickCommand = TileClickCommand,
            Margin = new Thickness(2),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch
        };
        Grid.SetRow(host, row);
        Grid.SetColumn(host, column);
        Grid.SetRowSpan(host, rowSpan);
        Grid.SetColumnSpan(host, columnSpan);
        LayoutSurface.Children.Add(host);
    }

    private void ApplyTileClickCommandToChildren()
    {
        foreach (var child in LayoutSurface.Children)
        {
            var multiviewTile = child switch
            {
                BroadcastMultiviewTile direct => direct,
                AspectRatioHost { Child: BroadcastMultiviewTile nested } => nested,
                _ => null
            };

            if (multiviewTile is not null)
            {
                multiviewTile.TileClickCommand = TileClickCommand;
            }
        }
    }
}
