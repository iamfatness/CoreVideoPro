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

    public ShowMultiviewHost()
    {
        InitializeComponent();
        Loaded += (_, _) => ScheduleRebuildLayout();
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
            if (tiles.Count == 3)
            {
                (row, column) = index switch
                {
                    0 => (0, 0),
                    1 => (0, 1),
                    _ => (1, 1)
                };
                AddTile(tile, row, column, index == 0 ? 2 : 1, 1);
                continue;
            }

            if (tiles.Count == 5)
            {
                (row, column) = index switch
                {
                    0 => (0, 0),
                    1 => (0, 1),
                    2 => (0, 2),
                    3 => (1, 1),
                    _ => (1, 2)
                };
                AddTile(tile, row, column, index == 0 ? 2 : 1, 1);
                continue;
            }

            AddTile(tile, row, column);
        }
    }

    private static (int Columns, int Rows) ResolveGridShape(int tileCount) =>
        tileCount switch
        {
            1 => (1, 1),
            2 => (2, 1),
            3 => (2, 2),
            <= 4 => (2, 2),
            5 => (3, 2),
            <= 6 => (3, 2),
            <= 8 => (4, 2),
            _ => (5, 2)
        };

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
