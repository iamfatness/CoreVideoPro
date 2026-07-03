using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// Presents the core-composited multiview as ONE GPU shared-texture surface (a single
/// VideoSurfaceHost / swap chain — the proven program model) plus transparent overlays for click
/// targets and broadcast decorations (name labels, red/green tally borders, audio meters, and a
/// clock). The grid layout and content are baked into the texture by the core; the overlays only
/// map taps back to source identity and draw crisp XAML chrome, so they rebuild on a STRUCTURAL
/// layout change (rare) or when a toggle flips — never per video frame, and never per
/// active-speaker change. This avoids the per-tile CPU/swap-chain churn that fail-fasts the WinUI
/// (CoreMessagingXP 0xc000027b).
/// </summary>
public sealed partial class ShowMultiviewHost : UserControl
{
    private static readonly Color TallyProgramColor = Color.FromArgb(255, 235, 64, 52);   // red
    private static readonly Color TallyPreviewColor = Color.FromArgb(255, 46, 204, 113);  // green
    private static readonly Color TallyNeutralColor = Color.FromArgb(120, 150, 165, 155); // subtle
    private static readonly Color MeterFillColor = Color.FromArgb(255, 92, 214, 130);

    public static readonly DependencyProperty SurfaceProperty =
        DependencyProperty.Register(nameof(Surface), typeof(VideoSurfaceState), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnSurfaceChanged));

    public static readonly DependencyProperty TileRectsProperty =
        DependencyProperty.Register(nameof(TileRects), typeof(IReadOnlyList<MultiviewTile>), typeof(ShowMultiviewHost),
            new PropertyMetadata(null, OnTileRectsChanged));

    public static readonly DependencyProperty TileClickCommandProperty =
        DependencyProperty.Register(nameof(TileClickCommand), typeof(ICommand), typeof(ShowMultiviewHost),
            new PropertyMetadata(null));

    public static readonly DependencyProperty TileSwapCommandProperty =
        DependencyProperty.Register(nameof(TileSwapCommand), typeof(ICommand), typeof(ShowMultiviewHost),
            new PropertyMetadata(null));

    public static readonly DependencyProperty ShowLabelsProperty =
        DependencyProperty.Register(nameof(ShowLabels), typeof(bool), typeof(ShowMultiviewHost),
            new PropertyMetadata(true, OnDecorToggleChanged));

    public static readonly DependencyProperty ShowTallyProperty =
        DependencyProperty.Register(nameof(ShowTally), typeof(bool), typeof(ShowMultiviewHost),
            new PropertyMetadata(true, OnDecorToggleChanged));

    public static readonly DependencyProperty ShowMetersProperty =
        DependencyProperty.Register(nameof(ShowMeters), typeof(bool), typeof(ShowMultiviewHost),
            new PropertyMetadata(true, OnDecorToggleChanged));

    public static readonly DependencyProperty ShowClockProperty =
        DependencyProperty.Register(nameof(ShowClock), typeof(bool), typeof(ShowMultiviewHost),
            new PropertyMetadata(true, OnClockToggleChanged));

    public static readonly DependencyProperty MeterLevelLeftProperty =
        DependencyProperty.Register(nameof(MeterLevelLeft), typeof(int), typeof(ShowMultiviewHost),
            new PropertyMetadata(0, OnMeterLevelChanged));

    public static readonly DependencyProperty MeterLevelRightProperty =
        DependencyProperty.Register(nameof(MeterLevelRight), typeof(int), typeof(ShowMultiviewHost),
            new PropertyMetadata(0, OnMeterLevelChanged));

    private readonly List<MeterChannel> _meters = new();
    private DispatcherTimer? _clockTimer;
    private IReadOnlyList<MultiviewTile> _tiles = [];

    // Letterbox transform of the last PositionOverlay pass — used to hit-test drag points
    // back to normalized tile rects.
    private double _overlayOffsetX;
    private double _overlayOffsetY;
    private double _overlayDisplayedWidth;
    private double _overlayDisplayedHeight;

    // Drag-to-reorder state. A press on a source tile arms a potential drag; crossing the
    // movement threshold turns it into one (showing ghost + target highlight); releasing over a
    // DIFFERENT source tile raises TileSwapCommand. Small press-release stays a click (cue).
    private const double DragThresholdPixels = 10.0;
    private MultiviewTile? _dragOriginTile;
    private Windows.Foundation.Point _dragStartPoint;
    private bool _dragActive;
    private Border? _dragGhost;
    private Border? _dragTargetHighlight;
    private MultiviewTile? _dragTargetTile;

    public ShowMultiviewHost()
    {
        InitializeComponent();
        // Surface starts null — keep the host collapsed so only the EmptyState shows.
        MultiviewSurfaceHost.Visibility = Visibility.Collapsed;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;

        // Drag-to-reorder listeners. The per-tile click Buttons capture the pointer and mark
        // events handled, but captured-pointer events still BUBBLE through the ancestor chain —
        // so listen on the overlay canvas with handledEventsToo to observe the whole gesture
        // without disturbing the Buttons' click behavior.
        ClickOverlay.AddHandler(PointerPressedEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler(OnTilePointerPressed), true);
        ClickOverlay.AddHandler(PointerMovedEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler(OnTilePointerMoved), true);
        ClickOverlay.AddHandler(PointerReleasedEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler(OnTilePointerReleased), true);
        ClickOverlay.AddHandler(PointerCaptureLostEvent,
            new Microsoft.UI.Xaml.Input.PointerEventHandler(OnTilePointerCaptureLost), true);
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

    /// <summary>Raised with a <see cref="MultiviewTileSwapRequest"/> when one source tile is
    /// drag-dropped onto another — the EIC's reorder gesture. Optional; when unset, tiles
    /// simply aren't draggable.</summary>
    public ICommand? TileSwapCommand
    {
        get => (ICommand?)GetValue(TileSwapCommandProperty);
        set => SetValue(TileSwapCommandProperty, value);
    }

    public bool ShowLabels
    {
        get => (bool)GetValue(ShowLabelsProperty);
        set => SetValue(ShowLabelsProperty, value);
    }

    public bool ShowTally
    {
        get => (bool)GetValue(ShowTallyProperty);
        set => SetValue(ShowTallyProperty, value);
    }

    public bool ShowMeters
    {
        get => (bool)GetValue(ShowMetersProperty);
        set => SetValue(ShowMetersProperty, value);
    }

    public bool ShowClock
    {
        get => (bool)GetValue(ShowClockProperty);
        set => SetValue(ShowClockProperty, value);
    }

    /// <summary>Master (or PGM) audio meter level, 0-100, left channel.</summary>
    public int MeterLevelLeft
    {
        get => (int)GetValue(MeterLevelLeftProperty);
        set => SetValue(MeterLevelLeftProperty, value);
    }

    /// <summary>Master (or PGM) audio meter level, 0-100, right channel.</summary>
    public int MeterLevelRight
    {
        get => (int)GetValue(MeterLevelRightProperty);
        set => SetValue(MeterLevelRightProperty, value);
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        StartOrStopClock();
        RebuildDecorations();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e) => StopClock();

    private static void OnSurfaceChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            // Collapse the surface host until there's a multiview texture, so its internal
            // "waiting for media engine" placeholder doesn't overlap the EmptyState message.
            host.MultiviewSurfaceHost.Visibility =
                args.NewValue is VideoSurfaceState ? Visibility.Visible : Visibility.Collapsed;
            // The canvas size (texture dims) drives the letterbox transform, so reposition when
            // the surface handle changes.
            host.PositionOverlay();
        }
    }

    private static void OnTileRectsChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host._tiles = (args.NewValue as IReadOnlyList<MultiviewTile>) ?? [];
            host.RebuildOverlay();
            host.RebuildDecorations();
        }
    }

    private static void OnDecorToggleChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host.RebuildDecorations();
        }
    }

    private static void OnClockToggleChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        if (sender is ShowMultiviewHost host)
        {
            host.StartOrStopClock();
        }
    }

    private static void OnMeterLevelChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        // Cheap: just resize the existing fill rectangles — no rebuild, no allocation storm.
        if (sender is ShowMultiviewHost host)
        {
            host.ApplyMeterLevels();
        }
    }

    private void OnOverlaySizeChanged(object sender, SizeChangedEventArgs e) => PositionOverlay();

    // Builds ≤10 transparent click buttons (one per tile). Called only when the tile-rect LAYOUT
    // changes (structural), so there is no per-frame / per-active-speaker UI churn.
    private void RebuildOverlay()
    {
        ClickOverlay.Children.Clear();

        var tiles = _tiles.Take(10).ToList();
        EmptyState.Visibility = tiles.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

        foreach (var tile in tiles)
        {
            var button = new Button
            {
                Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                BorderThickness = new Thickness(0),
                Padding = new Thickness(0),
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                VerticalContentAlignment = VerticalAlignment.Stretch,
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

    // Builds per-tile broadcast decorations (tally border, name label bar, audio meter), each gated
    // by its toggle. Rebuilt only on a structural tile-rect change or a toggle flip — never per
    // frame. Meter values then update in place via ApplyMeterLevels (no rebuild).
    private void RebuildDecorations()
    {
        if (DecorOverlay is null)
        {
            return;
        }

        DecorOverlay.Children.Clear();
        _meters.Clear();

        var tiles = _tiles.Take(10).ToList();
        foreach (var tile in tiles)
        {
            var decor = new Grid { Tag = tile, IsHitTestVisible = false };

            if (ShowTally)
            {
                decor.Children.Add(BuildTallyBorder(tile));
            }

            if (ShowLabels)
            {
                var labelBar = BuildLabelBar(tile);
                if (labelBar is not null)
                {
                    decor.Children.Add(labelBar);
                }
            }

            if (ShowMeters && MultiviewOverlayFormatting.ShouldShowMeter(tile))
            {
                decor.Children.Add(BuildMeter(tile));
            }

            DecorOverlay.Children.Add(decor);
        }

        PositionOverlay();
        ApplyMeterLevels();
    }

    private static Border BuildTallyBorder(MultiviewTile tile)
    {
        var tally = MultiviewOverlayFormatting.ResolveTally(tile);
        var (color, thickness) = tally switch
        {
            MultiviewOverlayFormatting.TallyProgram => (TallyProgramColor, 3.0),
            MultiviewOverlayFormatting.TallyPreview => (TallyPreviewColor, 3.0),
            _ => (TallyNeutralColor, 1.0)
        };

        return new Border
        {
            BorderBrush = new SolidColorBrush(color),
            BorderThickness = new Thickness(thickness),
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            IsHitTestVisible = false,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch
        };
    }

    private static Border? BuildLabelBar(MultiviewTile tile)
    {
        var text = MultiviewOverlayFormatting.ResolveLabel(tile);
        if (string.IsNullOrWhiteSpace(text))
        {
            return null;
        }

        var tally = MultiviewOverlayFormatting.ResolveTally(tile);
        var barColor = tally switch
        {
            MultiviewOverlayFormatting.TallyProgram => Color.FromArgb(210, 150, 30, 26),
            MultiviewOverlayFormatting.TallyPreview => Color.FromArgb(210, 24, 120, 66),
            _ => Color.FromArgb(160, 0, 0, 0)
        };

        var label = new TextBlock
        {
            Text = text,
            FontSize = 11,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
            Margin = new Thickness(6, 2, 6, 2),
            IsHitTestVisible = false,
            TextTrimming = TextTrimming.CharacterEllipsis,
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Left
        };

        return new Border
        {
            Background = new SolidColorBrush(barColor),
            IsHitTestVisible = false,
            VerticalAlignment = VerticalAlignment.Bottom,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Child = label
        };
    }

    // A small two-channel (L/R) vertical meter anchored to the tile's bottom-right. The track
    // heights are set during positioning (we know the tile's pixel size then); the fill heights are
    // driven from the level DPs via ApplyMeterLevels.
    private Border BuildMeter(MultiviewTile tile)
    {
        var leftFill = new Rectangle
        {
            Width = 5,
            Fill = new SolidColorBrush(MeterFillColor),
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(0, 0, 1, 0)
        };
        var rightFill = new Rectangle
        {
            Width = 5,
            Fill = new SolidColorBrush(MeterFillColor),
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(1, 0, 0, 0)
        };

        var bars = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            VerticalAlignment = VerticalAlignment.Bottom
        };
        bars.Children.Add(leftFill);
        bars.Children.Add(rightFill);

        var container = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(150, 6, 10, 8)),
            CornerRadius = new CornerRadius(2),
            Padding = new Thickness(2),
            IsHitTestVisible = false,
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(0, 0, 6, 6),
            Child = bars
        };

        _meters.Add(new MeterChannel(container, leftFill, rightFill));
        return container;
    }

    // Maps each normalized tile rect into overlay pixel space through the SAME uniform letterbox
    // (scale + center) the swap chain uses (Direct3D11InteropService.ApplyPanelTransform).
    private void PositionOverlay()
    {
        var overlayWidth = ClickOverlay?.ActualWidth ?? 0;
        var overlayHeight = ClickOverlay?.ActualHeight ?? 0;
        if (overlayWidth <= 0 || overlayHeight <= 0)
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

        _overlayOffsetX = offsetX;
        _overlayOffsetY = offsetY;
        _overlayDisplayedWidth = displayedWidth;
        _overlayDisplayedHeight = displayedHeight;

        PositionCanvasChildren(ClickOverlay, offsetX, offsetY, displayedWidth, displayedHeight);
        PositionCanvasChildren(DecorOverlay, offsetX, offsetY, displayedWidth, displayedHeight);

        // Size the meter tracks relative to the tile height now that we know pixel sizes.
        foreach (var meter in _meters)
        {
            if (meter.Container.Parent is FrameworkElement { Tag: MultiviewTile tile })
            {
                var tileHeight = Math.Max(0, tile.H * displayedHeight);
                meter.TrackHeight = Math.Clamp(tileHeight * 0.4, 12, 90);
            }
        }

        ApplyMeterLevels();
    }

    private static void PositionCanvasChildren(Canvas? canvas, double offsetX, double offsetY, double displayedWidth, double displayedHeight)
    {
        if (canvas is null)
        {
            return;
        }

        foreach (var child in canvas.Children)
        {
            if (child is not FrameworkElement { Tag: MultiviewTile tile } element)
            {
                continue;
            }

            element.Width = Math.Max(0, tile.W * displayedWidth);
            element.Height = Math.Max(0, tile.H * displayedHeight);
            Canvas.SetLeft(element, offsetX + tile.X * displayedWidth);
            Canvas.SetTop(element, offsetY + tile.Y * displayedHeight);
        }
    }

    private void ApplyMeterLevels()
    {
        if (_meters.Count == 0)
        {
            return;
        }

        var left = Math.Clamp(MeterLevelLeft / 100.0, 0.0, 1.0);
        var right = Math.Clamp(MeterLevelRight / 100.0, 0.0, 1.0);
        foreach (var meter in _meters)
        {
            var track = meter.TrackHeight;
            meter.LeftFill.Height = track * left;
            meter.RightFill.Height = track * right;
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

    private void StartOrStopClock()
    {
        if (ShowClock)
        {
            ClockChrome.Visibility = Visibility.Visible;
            UpdateClock();
            if (_clockTimer is null)
            {
                _clockTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
                _clockTimer.Tick += (_, _) => UpdateClock();
            }

            _clockTimer.Start();
        }
        else
        {
            StopClock();
            ClockChrome.Visibility = Visibility.Collapsed;
        }
    }

    private void StopClock() => _clockTimer?.Stop();

    private void UpdateClock() => ClockText.Text = MultiviewOverlayFormatting.FormatClock(DateTime.Now);

    // Resolves the source identity the tile-click command (PreviewMultiviewTile / BuildSoloRoute)
    // expects: a Participant.Id of the raw participant id for Zoom, or "capture:{id}" / "media:{id}".
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

    // ----- Drag-to-reorder (EIC rearranges the source wall) -----

    private static bool IsDraggableSource(MultiviewTile tile) =>
        string.Equals(tile.Role, "source", StringComparison.Ordinal) && tile.Slot >= 0;

    // Maps an overlay-space point back through the letterbox transform to the source tile
    // under it (PGM/PVW cells are not reorder targets).
    private MultiviewTile? HitTestSourceTile(Windows.Foundation.Point point)
    {
        if (_overlayDisplayedWidth <= 0 || _overlayDisplayedHeight <= 0)
        {
            return null;
        }

        var nx = (point.X - _overlayOffsetX) / _overlayDisplayedWidth;
        var ny = (point.Y - _overlayOffsetY) / _overlayDisplayedHeight;
        foreach (var tile in _tiles)
        {
            if (IsDraggableSource(tile) &&
                nx >= tile.X && nx <= tile.X + tile.W &&
                ny >= tile.Y && ny <= tile.Y + tile.H)
            {
                return tile;
            }
        }

        return null;
    }

    private void OnTilePointerPressed(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (TileSwapCommand is null)
        {
            return;
        }

        var point = e.GetCurrentPoint(ClickOverlay).Position;
        _dragOriginTile = HitTestSourceTile(point);
        _dragStartPoint = point;
        _dragActive = false;
    }

    private void OnTilePointerMoved(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (_dragOriginTile is null)
        {
            return;
        }

        var point = e.GetCurrentPoint(ClickOverlay).Position;
        if (!_dragActive)
        {
            var dx = point.X - _dragStartPoint.X;
            var dy = point.Y - _dragStartPoint.Y;
            if ((dx * dx) + (dy * dy) < DragThresholdPixels * DragThresholdPixels)
            {
                return;
            }

            _dragActive = true;
            // Steal the pointer capture from the tile Button: the canvas then reliably receives
            // the rest of the gesture (moves + release), and the Button can no longer raise a
            // stray Click — a drag is a drag, not a cue. (The Button's own capture-lost is
            // ignored below: only the CANVAS losing capture cancels the drag.)
            ClickOverlay.CapturePointer(e.Pointer);
            BuildDragVisuals(_dragOriginTile);
        }

        UpdateDragVisuals(point);
    }

    private void OnTilePointerReleased(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        var origin = _dragOriginTile;
        var wasDragging = _dragActive;
        var point = e.GetCurrentPoint(ClickOverlay).Position;
        if (wasDragging)
        {
            ClickOverlay.ReleasePointerCapture(e.Pointer);
        }

        ClearDragState();

        if (!wasDragging || origin is null)
        {
            return;
        }

        var target = HitTestSourceTile(point);
        if (target is null || target.Slot == origin.Slot)
        {
            return;
        }

        // Tile Slot is 0-based (ShowInput SlotNumber - 1).
        TileSwapCommand?.Execute(new MultiviewTileSwapRequest(origin.Slot + 1, target.Slot + 1));
    }

    private void OnTilePointerCaptureLost(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        // The tile Button losing capture is EXPECTED mid-drag (we steal it on activation).
        // Only the canvas itself losing capture (window deactivation, etc.) cancels the drag.
        if (ReferenceEquals(e.OriginalSource, ClickOverlay))
        {
            ClearDragState();
        }
    }

    private void BuildDragVisuals(MultiviewTile origin)
    {
        DragOverlay.Children.Clear();

        // Drop-target highlight: a thick accent border snapped to the hovered tile.
        _dragTargetHighlight = new Border
        {
            BorderBrush = new SolidColorBrush(Color.FromArgb(255, 61, 220, 151)),
            BorderThickness = new Thickness(3),
            Background = new SolidColorBrush(Color.FromArgb(50, 61, 220, 151)),
            CornerRadius = new CornerRadius(2),
            Visibility = Visibility.Collapsed
        };
        DragOverlay.Children.Add(_dragTargetHighlight);

        // Ghost: a compact floating chip with the dragged source's label.
        var ghostLabel = new TextBlock
        {
            Text = string.IsNullOrWhiteSpace(origin.Label) ? $"Input {origin.Slot + 1}" : origin.Label,
            FontSize = 12,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Microsoft.UI.Colors.White),
            Margin = new Thickness(10, 5, 10, 5)
        };
        _dragGhost = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(225, 16, 42, 32)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(255, 61, 220, 151)),
            BorderThickness = new Thickness(1.5),
            CornerRadius = new CornerRadius(4),
            Child = ghostLabel
        };
        DragOverlay.Children.Add(_dragGhost);
    }

    private void UpdateDragVisuals(Windows.Foundation.Point point)
    {
        if (_dragGhost is not null)
        {
            Canvas.SetLeft(_dragGhost, point.X + 14);
            Canvas.SetTop(_dragGhost, point.Y + 10);
        }

        var target = HitTestSourceTile(point);
        if (!ReferenceEquals(target, _dragTargetTile))
        {
            _dragTargetTile = target;
        }

        if (_dragTargetHighlight is null)
        {
            return;
        }

        if (target is null || _dragOriginTile is null || target.Slot == _dragOriginTile.Slot)
        {
            _dragTargetHighlight.Visibility = Visibility.Collapsed;
            return;
        }

        _dragTargetHighlight.Width = Math.Max(0, target.W * _overlayDisplayedWidth);
        _dragTargetHighlight.Height = Math.Max(0, target.H * _overlayDisplayedHeight);
        Canvas.SetLeft(_dragTargetHighlight, _overlayOffsetX + target.X * _overlayDisplayedWidth);
        Canvas.SetTop(_dragTargetHighlight, _overlayOffsetY + target.Y * _overlayDisplayedHeight);
        _dragTargetHighlight.Visibility = Visibility.Visible;
    }

    private void ClearDragState()
    {
        _dragOriginTile = null;
        _dragActive = false;
        _dragGhost = null;
        _dragTargetHighlight = null;
        _dragTargetTile = null;
        DragOverlay.Children.Clear();
    }

    private sealed class MeterChannel
    {
        public MeterChannel(Border container, Rectangle leftFill, Rectangle rightFill)
        {
            Container = container;
            LeftFill = leftFill;
            RightFill = rightFill;
        }

        public Border Container { get; }

        public Rectangle LeftFill { get; }

        public Rectangle RightFill { get; }

        public double TrackHeight { get; set; } = 40;
    }
}
