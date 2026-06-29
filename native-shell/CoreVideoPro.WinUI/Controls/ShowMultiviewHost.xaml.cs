using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// Presents the core-composited multiview as ONE GPU shared-texture surface (a single
/// VideoSurfaceHost / swap chain — the proven program model) plus a transparent overlay of
/// click targets + labels. The grid layout, content, and active-speaker border are all baked
/// into the texture by the core; the overlay only maps taps back to the source identity, so it
/// rebuilds on a structural layout change (rare) — never per frame, and never per active-speaker
/// change. This replaces the old per-participant CPU tiles / per-tile swap chains that fail-fast
/// the WinUI (CoreMessagingXP 0xc000027b) under roster/active-speaker churn.
/// </summary>
public sealed partial class ShowMultiviewHost : UserControl
{
    public static readonly DependencyProperty SurfaceProperty =
        DependencyProperty.Register(nameof(Surface), typeof(VideoSurfaceState), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnSurfaceChanged));

    public static readonly DependencyProperty TileRectsProperty =
        DependencyProperty.Register(nameof(TileRects), typeof(IReadOnlyList<MultiviewTile>), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnTileRectsChanged));

    public static readonly DependencyProperty TileClickCommandProperty =
        DependencyProperty.Register(nameof(TileClickCommand), typeof(ICommand), typeof(ShowMultiviewHost),
            new PropertyMetadata(null));

    private IReadOnlyList<MultiviewTile> _tiles = [];

    public ShowMultiviewHost()
    {
        InitializeComponent();
        // Surface starts null — keep the host collapsed so only the EmptyState shows.
        MultiviewSurfaceHost.Visibility = Visibility.Collapsed;
    }

    public VideoSurfaceState? Surface
    {
        get => (VideoSurfaceState?)GetValue(SurfaceProperty);
        set => SetValue(SurfaceProperty, value);
    }

    public IReadOnlyList<MultiviewTile>? TileRects
    {
        get => (IReadOnlyList<MultiviewTile>?)GetValue(TileRectsProperty);
        set => SetValue(TileRectsProperty, value);
    }

    public ICommand? TileClickCommand
    {
        get => (ICommand?)GetValue(TileClickCommandProperty);
        set => SetValue(TileClickCommandProperty, value);
    }

    private static void OnSurfaceChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            // Collapse the surface host until there's a multiview texture, so its internal
            // "waiting for media engine" placeholder doesn't overlap the EmptyState message
            // (the two were stacking on top of each other when nothing was assigned).
            host.MultiviewSurfaceHost.Visibility =
                args.NewValue is VideoSurfaceState ? Visibility.Visible : Visibility.Collapsed;
            // The canvas size (texture dims) drives the letterbox transform, so reposition when
            // the surface handle changes (a rebuild is cheap — only repositions existing buttons).
            host.PositionOverlay();
        }
    }

    private static void OnTileRectsChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host._tiles = (args.NewValue as IReadOnlyList<MultiviewTile>) ?? [];
            host.RebuildOverlay();
        }
    }

    private void OnOverlaySizeChanged(object sender, SizeChangedEventArgs e) => PositionOverlay();

    // Builds ≤10 transparent click buttons (one per tile) + labels. Called only when the tile-rect
    // LAYOUT changes (structural), so there is no per-frame / per-active-speaker UI churn.
    private void RebuildOverlay()
    {
        ClickOverlay.Children.Clear();

        var tiles = _tiles.Take(10).ToList();
        EmptyState.Visibility = tiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        foreach (var tile in tiles)
        {
            var label = new TextBlock
            {
                Text = string.IsNullOrWhiteSpace(tile.Label) ? string.Empty : tile.Label,
                FontSize = 11,
                Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
                Margin = new Thickness(6, 4, 6, 4),
                IsHitTestVisible = false,
                VerticalAlignment = VerticalAlignment.Bottom,
                HorizontalAlignment = HorizontalAlignment.Left,
                TextTrimming = TextTrimming.CharacterEllipsis
            };

            var labelChrome = new Border
            {
                Background = new SolidColorBrush(Color.FromArgb(140, 0, 0, 0)),
                CornerRadius = new CornerRadius(3),
                VerticalAlignment = VerticalAlignment.Bottom,
                HorizontalAlignment = HorizontalAlignment.Left,
                Margin = new Thickness(4),
                IsHitTestVisible = false,
                Child = label
            };

            var content = new Grid();
            content.Children.Add(labelChrome);

            var button = new Button
            {
                Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                BorderThickness = new Thickness(0),
                Padding = new Thickness(0),
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                VerticalContentAlignment = VerticalAlignment.Stretch,
                Content = content,
                CommandParameter = ToSurfaceTile(tile),
                Tag = tile
            };
            button.SetBinding(Button.CommandProperty, new Microsoft.UI.Xaml.Data.Binding
            {
                Source = this,
                Path = new PropertyPath(nameof(TileClickCommand))
            });

            ClickOverlay.Children.Add(button);
        }

        PositionOverlay();
    }

    // Maps each normalized tile rect into overlay pixel space through the SAME uniform letterbox
    // (scale + center) the swap chain uses (Direct3D11InteropService.ApplyPanelTransform): the
    // displayed canvas rect = the largest canvas-aspect rect that fits the overlay, centered.
    private void PositionOverlay()
    {
        var overlayWidth = ClickOverlay.ActualWidth;
        var overlayHeight = ClickOverlay.ActualHeight;
        if (overlayWidth <= 0 || overlayHeight <= 0 || ClickOverlay.Children.Count == 0)
        {
            return;
        }

        var (canvasWidth, canvasHeight) = ResolveCanvasSize();
        if (canvasWidth <= 0 || canvasHeight <= 0)
        {
            return;
        }

        var scale = Math.Min(overlayWidth / canvasWidth, overlayHeight / canvasHeight);
        var displayedWidth = canvasWidth * scale;
        var displayedHeight = canvasHeight * scale;
        var offsetX = (overlayWidth - displayedWidth) / 2.0;
        var offsetY = (overlayHeight - displayedHeight) / 2.0;

        foreach (var child in ClickOverlay.Children)
        {
            if (child is not Button { Tag: MultiviewTile tile } button)
            {
                continue;
            }

            var left = offsetX + tile.X * displayedWidth;
            var top = offsetY + tile.Y * displayedHeight;
            var width = Math.Max(0, tile.W * displayedWidth);
            var height = Math.Max(0, tile.H * displayedHeight);

            button.Width = width;
            button.Height = height;
            Canvas.SetLeft(button, left);
            Canvas.SetTop(button, top);
        }
    }

    private (double Width, double Height) ResolveCanvasSize()
    {
        var handle = Surface?.PendingSharedHandle;
        if (handle is { Width: > 0, Height: > 0 })
        {
            return (handle.Width, handle.Height);
        }

        var frame = Surface?.LastFrame;
        if (frame is { Width: > 0, Height: > 0 })
        {
            return (frame.Width, frame.Height);
        }

        // Default to the 16:9 production canvas if the texture dims are not yet known.
        return (1920.0, 1080.0);
    }

    // Resolves the source identity the tile-click command (PreviewMultiviewTile / BuildSoloRoute)
    // expects: a Participant.Id of the raw participant id for Zoom, or "capture:{id}" / "media:{id}".
    // The core stamps sourceId as "zoom:{pid}" / "capture:{id}" / "media:{id}".
    private static ParticipantSurfaceTile ToSurfaceTile(MultiviewTile tile)
    {
        var routingId = ResolveRoutingId(tile);
        return new ParticipantSurfaceTile
        {
            IsEmpty = string.IsNullOrWhiteSpace(routingId),
            SourceIndex = tile.Slot + 1,
            Participant = new Participant
            {
                Id = routingId,
                Name = tile.Label,
                Role = ParticipantRole.Guest,
                Health = FeedHealth.Live
            }
        };
    }

    private static string ResolveRoutingId(MultiviewTile tile)
    {
        if (!string.IsNullOrWhiteSpace(tile.SourceId))
        {
            if (tile.SourceId.StartsWith("zoom:", StringComparison.Ordinal))
            {
                return tile.SourceId["zoom:".Length..];
            }

            // capture:{deviceId} and media:{assetId} are already the routing identity.
            return tile.SourceId;
        }

        return tile.ParticipantId;
    }
}
