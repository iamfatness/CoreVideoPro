using System.Collections;
using CoreVideoPro.WinUI.Models;
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
        LayoutSurface.ColumnDefinitions.Clear();

        var tiles = (Tiles ?? Array.Empty<ParticipantSurfaceTile>())
            .Cast<ParticipantSurfaceTile>()
            .Where(tile => !tile.IsEmpty)
            .Take(2)
            .ToList();

        EmptyState.Visibility = tiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        if (tiles.Count == 0)
        {
            LayoutSurface.Children.Add(EmptyState);
            return;
        }

        for (var column = 0; column < tiles.Count; column++)
        {
            LayoutSurface.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var tile = tiles[column];
            var host = new AspectRatioHost
            {
                AspectWidth = 16,
                AspectHeight = 9,
                Child = new BroadcastMultiviewTile
                {
                    Tile = tile,
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    VerticalAlignment = VerticalAlignment.Stretch
                }
            };
            Grid.SetColumn(host, column);
            LayoutSurface.Children.Add(host);
        }
    }
}