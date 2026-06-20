using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using System.Windows.Input;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class BroadcastMultiviewTile : UserControl
{
    public static readonly DependencyProperty TileProperty =
        DependencyProperty.Register(
            nameof(Tile),
            typeof(ParticipantSurfaceTile),
            typeof(BroadcastMultiviewTile),
            new PropertyMetadata(null, OnTileChanged));

    public static readonly DependencyProperty TileClickCommandProperty =
        DependencyProperty.Register(
            nameof(TileClickCommand),
            typeof(ICommand),
            typeof(BroadcastMultiviewTile),
            new PropertyMetadata(null));

    public BroadcastMultiviewTile()
    {
        InitializeComponent();
    }

    public ParticipantSurfaceTile? Tile
    {
        get => (ParticipantSurfaceTile?)GetValue(TileProperty);
        set => SetValue(TileProperty, value);
    }

    public ICommand? TileClickCommand
    {
        get => (ICommand?)GetValue(TileClickCommandProperty);
        set => SetValue(TileClickCommandProperty, value);
    }

    public string SourceLabel => Tile is null ? "—" : Tile.SourceLabel;

    public string ParticipantName => Tile?.Participant.Name ?? "No source";

    public string RoleBadge => Tile?.Participant.RoleBadge ?? string.Empty;

    public VideoSurfaceState? Surface => Tile?.Surface;

    public bool ShowTalkingBadge => Tile?.Participant.IsActiveSpeaker == true;

    private static void OnTileChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is BroadcastMultiviewTile tile)
        {
            tile.Bindings.Update();
            tile.ApplyTallyChrome();
        }
    }

    private void OnTileTapped(object sender, TappedRoutedEventArgs e)
    {
        if (Tile is not { IsEmpty: false } tile)
        {
            return;
        }

        if (TileClickCommand?.CanExecute(tile) == true)
        {
            TileClickCommand.Execute(tile);
            e.Handled = true;
        }
    }

    private void ApplyTallyChrome()
    {
        if (Tile?.IsEmpty == true)
        {
            TileChrome.Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 8, 10, 12));
            TileChrome.BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 28, 32, 38));
            TileChrome.BorderThickness = new Thickness(1);
            TileChrome.Opacity = 0.72;
            return;
        }

        TileChrome.Opacity = 1;
        if (Tile?.Participant.IsActiveSpeaker == true)
        {
            TileChrome.BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 168, 92));
            TileChrome.BorderThickness = new Thickness(2);
            return;
        }

        TileChrome.BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 42, 48, 56));
        TileChrome.BorderThickness = new Thickness(1);
    }
}
