using System.Collections;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class ShowMultiviewHost : UserControl
{
    public static readonly DependencyProperty TilesProperty =
        DependencyProperty.Register(nameof(Tiles), typeof(IEnumerable), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnTilesChanged));

    public ShowMultiviewHost()
    {
        InitializeComponent();
        Loaded += (_, _) => RebuildLayout();
    }

    public IEnumerable? Tiles
    {
        get => (IEnumerable?)GetValue(TilesProperty);
        set => SetValue(TilesProperty, value);
    }

    private static void OnTilesChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host.RebuildLayout();
        }
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => RebuildLayout();

    private void RebuildLayout()
    {
        LayoutSurface.Children.Clear();
        LayoutSurface.RowDefinitions.Clear();
        LayoutSurface.ColumnDefinitions.Clear();

        var tiles = (Tiles ?? Array.Empty<ParticipantSurfaceTile>())
            .Cast<ParticipantSurfaceTile>()
            .Where(tile => !tile.IsEmpty)
            .Take(ShowInputRosterService.MaxMultiviewBoxes)
            .ToList();

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
            var host = new AspectRatioHost
            {
                AspectWidth = 16,
                AspectHeight = 9,
                Margin = new Thickness(2),
                Child = new BroadcastMultiviewTile
                {
                    Tile = tile,
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    VerticalAlignment = VerticalAlignment.Stretch
                }
            };
            Grid.SetRow(host, row);
            Grid.SetColumn(host, column);
            LayoutSurface.Children.Add(host);
        }
    }

    private static (int Columns, int Rows) ResolveGridShape(int tileCount) =>
        tileCount switch
        {
            1 => (1, 1),
            2 => (2, 1),
            <= 4 => (2, 2),
            <= 6 => (3, 2),
            <= 8 => (4, 2),
            _ => (5, 2)
        };
}