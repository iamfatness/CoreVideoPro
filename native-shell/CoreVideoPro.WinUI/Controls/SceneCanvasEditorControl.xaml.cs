using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Text;
using Windows.Foundation;
using Windows.UI;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class SceneCanvasEditorControl : UserControl
{
    private const double DesignWidth = 1600;
    private const double DesignHeight = 900;
    private const double MinLayerSize = 0.08;

    private readonly Dictionary<int, Border> _layerFrames = new();
    private SceneCanvasLayerViewModel? _selectedLayer;
    private SceneCanvasLayerViewModel? _dragLayer;
    private string _dragMode = "move";
    private Point _dragStartPointer;
    private Point _dragStartControlPointer;
    private bool _dragMoved;
    private NormalizedCanvasRect? _dragStartRect;
    private double _dragStartSourceOffsetX;
    private double _dragStartSourceOffsetY;
    private bool _hasComposite;
    private MediaAsset? _backgroundAsset;

    public event EventHandler<string>? PresetRequested;
    public event EventHandler<SceneCanvasLayerViewModel>? LayerChanged;
    private long _lastCompositeDragUpdate;
    public event EventHandler<bool>? InteractionChanged;

    public SceneCanvasEditorControl()
    {
        InitializeComponent();
        BuildPresetButtons();
        SizeChanged += OnEditorSizeChanged;
        // S3 keyboard nudge: the control takes focus when a layer is pressed so
        // arrow keys (Shift = coarse) move the selected box.
        IsTabStop = true;
        KeyDown += OnEditorKeyDown;
    }

    public bool IsInteracting => _dragLayer is not null;

    public void SetCompositeSurface(VideoSurfaceState? surface)
    {
        var hasComposite = SceneCanvasPresentationRules.HasComposite(surface);
        CompositePreview.SurfaceState = hasComposite ? surface : null;
        CompositePreview.Visibility = hasComposite ? Visibility.Visible : Visibility.Collapsed;
        if (_hasComposite == hasComposite) return;
        _hasComposite = hasComposite;
        ApplyBackground();
        foreach (var frame in _layerFrames.Values)
        {
            if (frame.Tag is SceneCanvasLayerViewModel layer)
                UpdateLayerSurface(frame, layer.Surface);
        }
    }

    public void SetBackground(MediaAsset? asset)
    {
        _backgroundAsset = asset;
        ApplyBackground();
    }

    private void ApplyBackground()
    {
        var asset = _hasComposite ? null : _backgroundAsset;
        if (asset is null)
        {
            BackgroundPreview.Visibility = Visibility.Collapsed;
            BackgroundPreview.FilePath = null;
            BackgroundPreview.Kind = null;
            BackgroundPreview.IsPlaying = false;
            BackgroundPreview.PlaybackKey = null;
            return;
        }

        BackgroundPreview.FilePath = asset.FilePath;
        BackgroundPreview.Kind = asset.Kind;
        BackgroundPreview.PlaybackKey = $"scene-background:{asset.Id}";
        BackgroundPreview.IsLooping = true;
        BackgroundPreview.IsPlaying = asset.Kind.Equals("video", StringComparison.OrdinalIgnoreCase);
        BackgroundPreview.Visibility = Visibility.Visible;
    }

    private void OnEditorSizeChanged(object sender, SizeChangedEventArgs e) =>
        ResizeCanvasViewport();

    private void ResizeCanvasViewport()
    {
        var viewportWidth = CanvasViewport.ActualWidth > 0
            ? CanvasViewport.ActualWidth
            : ActualWidth;
        var viewportHeight = CanvasViewport.ActualHeight > 0
            ? CanvasViewport.ActualHeight
            : ActualHeight;

        var fitSize = SceneCanvasViewportService.ResolveFitSize(
            viewportWidth,
            viewportHeight,
            DesignWidth / DesignHeight,
            SceneCanvasViewportService.DefaultEditorMaxHeight);
        if (fitSize.Width <= 0 || fitSize.Height <= 0)
        {
            return;
        }

        CanvasViewbox.Width = fitSize.Width;
        CanvasViewbox.Height = fitSize.Height;
    }

    public void SetLayers(IReadOnlyList<SceneCanvasLayerViewModel>? layers, SceneCanvasLayerViewModel? selectedLayer)
    {
        if (_dragLayer is not null)
        {
            return;
        }

        ResizeCanvasViewport();
        if (_selectedLayer is null || layers?.Contains(_selectedLayer) != true)
        {
            _selectedLayer = selectedLayer;
        }
        SyncLayers(layers ?? Array.Empty<SceneCanvasLayerViewModel>());
    }

    private void OnPresetClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button button && button.Tag is string preset)
        {
            PresetRequested?.Invoke(this, preset);
        }
    }

    private void BuildPresetButtons()
    {
        PresetButtonPanel.Children.Clear();
        Resources.TryGetValue("OperatorGhostButton", out var style);
        var buttonStyle = style as Style;
        foreach (var option in SceneCanvasLayoutService.PresetOptions)
        {
            var button = new Button
            {
                Content = option.Label,
                Tag = option.Value
            };
            if (buttonStyle is not null)
            {
                button.Style = buttonStyle;
            }

            button.Click += OnPresetClick;
            PresetButtonPanel.Children.Add(button);
        }
    }

    private void SyncLayers(IReadOnlyList<SceneCanvasLayerViewModel> layers)
    {
        CanvasHintText.Text = layers.Count == 0
            ? "Add sources from the list below, then drag them anywhere on the frame."
            : "Drag boxes to place them. Drag the center target to pan the source inside its box.";

        var activeIndices = layers.Select(layer => layer.LayerIndex).ToHashSet();
        foreach (var staleIndex in _layerFrames.Keys.Where(index => !activeIndices.Contains(index)).ToList())
        {
            if (_layerFrames.Remove(staleIndex, out var staleFrame))
            {
                LayerCanvas.Children.Remove(staleFrame);
            }
        }

        foreach (var layer in layers.OrderBy(entry => entry.LayerIndex))
        {
            if (!_layerFrames.TryGetValue(layer.LayerIndex, out var frame))
            {
                frame = CreateLayerFrame(layer);
                _layerFrames[layer.LayerIndex] = frame;
                LayerCanvas.Children.Add(frame);
            }
            else
            {
                frame.Tag = layer;
                UpdateLayerLabel(frame, layer.LayerLabel);
            }

            UpdateLayerSurface(frame, layer.Surface);
            UpdateLayerVisuals(frame, layer);
            ApplyLayerGeometry(frame, layer);
            ApplySelectionStyle(frame, ReferenceEquals(layer, _selectedLayer));
        }
    }

    private Border CreateLayerFrame(SceneCanvasLayerViewModel layer)
    {
        var frame = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(48, 68, 193, 161)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(160, 68, 193, 161)),
            BorderThickness = new Thickness(1.5),
            CornerRadius = new CornerRadius(6),
            Tag = layer
        };

        var content = new Grid();
        var videoHost = new VideoSurfaceHost
        {
            Name = "LayerPreview",
            SurfaceState = _hasComposite ? null : layer.Surface,
            Visibility = _hasComposite ? Visibility.Collapsed : Visibility.Visible,
            SourceFit = layer.FitMode,
            SourceScale = layer.SourceScale,
            SourceOffsetX = layer.SourceOffsetX,
            SourceOffsetY = layer.SourceOffsetY,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            IsHitTestVisible = false
        };
        content.Children.Add(videoHost);

        var labelChrome = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(180, 0, 0, 0)),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            Padding = new Thickness(8, 5, 8, 5),
            CornerRadius = new CornerRadius(0, 0, 4, 0)
        };
        var label = new TextBlock
        {
            Name = "LayerLabel",
            Text = layer.LayerLabel,
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Color.FromArgb(255, 237, 244, 239)),
            VerticalAlignment = VerticalAlignment.Top
        };
        labelChrome.Child = label;
        content.Children.Add(labelChrome);

        // S3b: a resize grip in every corner (tags name the corner; the drag
        // math moves that corner's two edges). Bottom-right keeps the legacy
        // "resize" tag. Hold Shift while dragging to lock the aspect ratio.
        content.Children.Add(CreateResizeGrip("resize", HorizontalAlignment.Right, VerticalAlignment.Bottom));
        content.Children.Add(CreateResizeGrip("resize-sw", HorizontalAlignment.Left, VerticalAlignment.Bottom));
        content.Children.Add(CreateResizeGrip("resize-ne", HorizontalAlignment.Right, VerticalAlignment.Top));
        content.Children.Add(CreateResizeGrip("resize-nw", HorizontalAlignment.Left, VerticalAlignment.Top));

        var panHandle = new Border
        {
            Width = 30,
            Height = 30,
            Background = new SolidColorBrush(Color.FromArgb(170, 10, 16, 22)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(230, 68, 193, 161)),
            BorderThickness = new Thickness(1.5),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            CornerRadius = new CornerRadius(15),
            Tag = "pan-source"
        };
        ToolTipService.SetToolTip(panHandle, "Pan source inside box");
        panHandle.Child = new FontIcon
        {
            Glyph = "\uE740",
            FontSize = 14,
            Foreground = new SolidColorBrush(Color.FromArgb(255, 237, 244, 239)),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            IsHitTestVisible = false
        };
        content.Children.Add(panHandle);

        frame.Child = content;
        frame.PointerPressed += OnLayerPointerPressed;
        frame.PointerMoved += OnLayerPointerMoved;
        frame.PointerReleased += OnLayerPointerReleased;
        frame.PointerCanceled += OnLayerPointerReleased;
        return frame;
    }

    private static Border CreateResizeGrip(string tag, HorizontalAlignment horizontal, VerticalAlignment vertical)
    {
        var grip = new Border
        {
            Width = 14,
            Height = 14,
            Background = new SolidColorBrush(Color.FromArgb(220, 240, 168, 92)),
            HorizontalAlignment = horizontal,
            VerticalAlignment = vertical,
            Margin = new Thickness(6),
            CornerRadius = new CornerRadius(3),
            Tag = tag
        };
        ToolTipService.SetToolTip(grip, "Resize (Shift = keep aspect)");
        return grip;
    }

    private static void UpdateLayerLabel(Border frame, string label)
    {
        if (frame.Child is not Grid content)
        {
            return;
        }

        foreach (var child in content.Children)
        {
            if (child is Border { Child: TextBlock textBlock } && textBlock.Name == "LayerLabel")
            {
                textBlock.Text = label;
                return;
            }
        }
    }

    private void UpdateLayerSurface(Border frame, VideoSurfaceState surface)
    {
        frame.Background = new SolidColorBrush(_hasComposite
            ? Color.FromArgb(0, 0, 0, 0)
            : Color.FromArgb(48, 68, 193, 161));
        if (frame.Child is not Grid content)
        {
            return;
        }

        foreach (var child in content.Children)
        {
            if (child is VideoSurfaceHost host)
            {
                host.Visibility = _hasComposite ? Visibility.Collapsed : Visibility.Visible;
                var shownSurface = _hasComposite ? null : surface;
                if (ReferenceEquals(host.SurfaceState, shownSurface)) return;
                host.SurfaceState = shownSurface;
                host.RefreshSourceFraming();
                return;
            }
        }
    }

    private static void UpdateLayerVisuals(Border frame, SceneCanvasLayerViewModel layer)
    {
        frame.BorderBrush = BrushForBorderStyle(layer.BorderStyle, layer.BorderColor);
        frame.BorderThickness = new Thickness(layer.BorderStyle == "none" ? 0 : Math.Clamp(layer.BorderThickness, 0, 12));

        if (frame.Child is not Grid content)
        {
            return;
        }

        foreach (var child in content.Children)
        {
            if (child is VideoSurfaceHost host)
            {
                host.SourceFit = layer.FitMode;
                host.SourceScale = layer.SourceScale;
                host.SourceOffsetX = layer.SourceOffsetX;
                host.SourceOffsetY = layer.SourceOffsetY;
                host.RefreshSourceFraming();
                return;
            }
        }
    }

    private static void ApplyLayerGeometry(Border frame, SceneCanvasLayerViewModel layer)
    {
        Canvas.SetLeft(frame, layer.X * DesignWidth);
        Canvas.SetTop(frame, layer.Y * DesignHeight);
        frame.Width = layer.Width * DesignWidth;
        frame.Height = layer.Height * DesignHeight;
        RefreshLayerVideoHost(frame);
    }

    private static void RefreshLayerVideoHost(Border frame)
    {
        if (frame.Child is not Grid content)
        {
            return;
        }

        foreach (var child in content.Children)
        {
            if (child is VideoSurfaceHost host)
            {
                host.RefreshSourceFraming();
                return;
            }
        }
    }

    private void ApplySelectionStyle(Border frame, bool isSelected)
    {
        if (frame.Tag is not SceneCanvasLayerViewModel layer)
        {
            return;
        }

        frame.BorderBrush = isSelected
            ? new SolidColorBrush(Color.FromArgb(255, 68, 193, 161))
            : BrushForBorderStyle(layer.BorderStyle, layer.BorderColor);
        frame.BorderThickness = new Thickness(isSelected
            ? Math.Max(3, layer.BorderThickness)
            : layer.BorderStyle == "none"
                ? 0
                : Math.Clamp(layer.BorderThickness, 0, 12));
    }

    private void UpdateSelectionStyles()
    {
        foreach (var frame in _layerFrames.Values)
        {
            if (frame.Tag is SceneCanvasLayerViewModel layer)
            {
                var selected = ReferenceEquals(layer, _selectedLayer);
                ApplySelectionStyle(frame, selected);
                layer.IsSelected = selected;  // S3b: cards mirror canvas selection
            }
        }
    }

    // S3b: canvas↔card selection sync. Raised when a layer is selected by
    // pointer on the canvas; SelectLayer is the inbound direction (card tap).
    public event EventHandler<SceneCanvasLayerViewModel>? LayerSelected;

    public void SelectLayer(SceneCanvasLayerViewModel layer)
    {
        _selectedLayer = layer;
        UpdateSelectionStyles();
        Focus(FocusState.Programmatic);
    }

    private void OnLayerPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (sender is not Border frame || frame.Tag is not SceneCanvasLayerViewModel layer)
        {
            return;
        }

        _selectedLayer = layer;
        _dragLayer = layer;
        _dragStartPointer = e.GetCurrentPoint(LayerCanvas).Position;
        _dragStartControlPointer = e.GetCurrentPoint(this).Position;
        _dragMoved = false;
        _dragStartRect = new NormalizedCanvasRect
        {
            X = layer.X,
            Y = layer.Y,
            Width = layer.Width,
            Height = layer.Height
        };
        _dragStartSourceOffsetX = layer.SourceOffsetX;
        _dragStartSourceOffsetY = layer.SourceOffsetY;

        _dragMode = ResolveDragMode(e.OriginalSource, frame);
        frame.CapturePointer(e.Pointer);
        UpdateSelectionStyles();
        Focus(FocusState.Programmatic);  // arrow-key nudge targets this layer
        LayerSelected?.Invoke(this, layer);
        InteractionChanged?.Invoke(this, true);
        e.Handled = true;
    }

    private static string ResolveDragMode(object? originalSource, Border frame)
    {
        for (var current = originalSource as DependencyObject; current is not null; current = VisualTreeHelper.GetParent(current))
        {
            if (ReferenceEquals(current, frame))
            {
                break;
            }

            if (current is FrameworkElement { Tag: string tag } &&
                (tag.StartsWith("resize", StringComparison.Ordinal) || tag == "pan-source"))
            {
                return tag;
            }
        }

        return "move";
    }

    private void OnLayerPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (_dragLayer is null || _dragStartRect is null || sender is not Border frame)
        {
            return;
        }

        var controlPosition = e.GetCurrentPoint(this).Position;
        if (!_dragMoved && Math.Abs(controlPosition.X - _dragStartControlPointer.X) < 3 &&
            Math.Abs(controlPosition.Y - _dragStartControlPointer.Y) < 3)
        {
            e.Handled = true;
            return;
        }
        _dragMoved = true;
        var position = e.GetCurrentPoint(LayerCanvas).Position;
        var deltaX = (position.X - _dragStartPointer.X) / DesignWidth;
        var deltaY = (position.Y - _dragStartPointer.Y) / DesignHeight;

        if (_dragMode.StartsWith("resize", StringComparison.Ordinal))
        {
            // S3b: generic corner resize — the dragged corner moves its two
            // edges; the opposite corner stays anchored. Legacy "resize" = SE.
            var movesWest = _dragMode is "resize-sw" or "resize-nw";
            var movesNorth = _dragMode is "resize-ne" or "resize-nw";
            var left = _dragStartRect.X;
            var top = _dragStartRect.Y;
            var right = left + _dragStartRect.Width;
            var bottom = top + _dragStartRect.Height;

            var (guidesX, guidesY) = BuildSnapGuides(_dragLayer);
            if (movesWest)
            {
                left = Math.Clamp(left + deltaX, 0, right - MinLayerSize);
                left = Math.Clamp(left + SnapCorrection(left, guidesX), 0, right - MinLayerSize);
            }
            else
            {
                right = Math.Clamp(right + deltaX, left + MinLayerSize, 1);
                right = Math.Clamp(right + SnapCorrection(right, guidesX), left + MinLayerSize, 1);
            }

            if (movesNorth)
            {
                top = Math.Clamp(top + deltaY, 0, bottom - MinLayerSize);
                top = Math.Clamp(top + SnapCorrection(top, guidesY), 0, bottom - MinLayerSize);
            }
            else
            {
                bottom = Math.Clamp(bottom + deltaY, top + MinLayerSize, 1);
                bottom = Math.Clamp(bottom + SnapCorrection(bottom, guidesY), top + MinLayerSize, 1);
            }

            // Shift = aspect lock to the drag-start ratio: the dominant axis
            // wins, the other follows, growing from the anchored corner.
            if (IsShiftDown() && _dragStartRect.Width > 0 && _dragStartRect.Height > 0)
            {
                var aspect = _dragStartRect.Width / _dragStartRect.Height;
                var width = right - left;
                var height = bottom - top;
                if (Math.Abs(width - _dragStartRect.Width) >= Math.Abs(height - _dragStartRect.Height))
                {
                    height = Math.Clamp(width / aspect, MinLayerSize, movesNorth ? bottom : 1 - top);
                    width = height * aspect;
                }
                else
                {
                    width = Math.Clamp(height * aspect, MinLayerSize, movesWest ? right : 1 - left);
                    height = width / aspect;
                }

                if (movesWest) { left = right - width; } else { right = left + width; }
                if (movesNorth) { top = bottom - height; } else { bottom = top + height; }
            }

            _dragLayer.SetCanvasRect(left, top, right - left, bottom - top, notify: false);
        }
        else if (_dragMode == "pan-source")
        {
            var boxWidth = Math.Max(_dragStartRect.Width, MinLayerSize);
            var boxHeight = Math.Max(_dragStartRect.Height, MinLayerSize);
            var offset = SourceFramingLayoutService.ResolveOffsetAfterDrag(
                boxWidth * DesignWidth,
                boxHeight * DesignHeight,
                _dragLayer.Surface.FramingSourceWidth,
                _dragLayer.Surface.FramingSourceHeight,
                _dragLayer.FitMode,
                _dragLayer.SourceScale,
                _dragStartSourceOffsetX,
                _dragStartSourceOffsetY,
                deltaX * DesignWidth,
                deltaY * DesignHeight);
            _dragLayer.SetSourceOffset(offset.X, offset.Y, notify: false);
            UpdateLayerVisuals(frame, _dragLayer);
        }
        else
        {
            var x = Math.Clamp(_dragStartRect.X + deltaX, 0, 1 - _dragStartRect.Width);
            var y = Math.Clamp(_dragStartRect.Y + deltaY, 0, 1 - _dragStartRect.Height);
            // S3 snapping: left/right/center of the box snap to canvas guides
            // (0, 0.5, 1) and other layers' edges/centers; smallest correction
            // within threshold wins per axis.
            var (guidesX, guidesY) = BuildSnapGuides(_dragLayer);
            x = Math.Clamp(
                x + SmallestCorrection(guidesX, x, x + _dragStartRect.Width, x + _dragStartRect.Width / 2),
                0, 1 - _dragStartRect.Width);
            y = Math.Clamp(
                y + SmallestCorrection(guidesY, y, y + _dragStartRect.Height, y + _dragStartRect.Height / 2),
                0, 1 - _dragStartRect.Height);
            _dragLayer.SetCanvasRect(x, y, _dragStartRect.Width, _dragStartRect.Height, notify: false);
        }

        ApplyLayerGeometry(frame, _dragLayer);
        // The live image now comes from the core composite. Send bounded drag
        // updates so it follows the manipulation chrome; release flushes the end.
        var now = Environment.TickCount64;
        if (_hasComposite && now - _lastCompositeDragUpdate >= 50)
        {
            _lastCompositeDragUpdate = now;
            LayerChanged?.Invoke(this, _dragLayer);
        }
        e.Handled = true;
    }

    // ---- S3 snapping ----------------------------------------------------------
    private const double SnapThreshold = 0.015;

    private (List<double> X, List<double> Y) BuildSnapGuides(SceneCanvasLayerViewModel dragged)
    {
        var guidesX = new List<double> { 0, 0.5, 1 };
        var guidesY = new List<double> { 0, 0.5, 1 };
        foreach (var frame in _layerFrames.Values)
        {
            if (frame.Tag is not SceneCanvasLayerViewModel layer || ReferenceEquals(layer, dragged))
            {
                continue;
            }

            guidesX.Add(layer.X);
            guidesX.Add(layer.X + layer.Width);
            guidesX.Add(layer.X + layer.Width / 2);
            guidesY.Add(layer.Y);
            guidesY.Add(layer.Y + layer.Height);
            guidesY.Add(layer.Y + layer.Height / 2);
        }

        return (guidesX, guidesY);
    }

    // Correction that moves `target` onto the nearest guide within threshold.
    private static double SnapCorrection(double target, List<double> guides)
    {
        var best = 0.0;
        var bestDistance = SnapThreshold;
        foreach (var guide in guides)
        {
            var distance = Math.Abs(target - guide);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = guide - target;
            }
        }

        return best;
    }

    // Smallest correction among several box features (edges + center) so the
    // closest feature is the one that snaps.
    private static double SmallestCorrection(List<double> guides, params double[] targets)
    {
        var best = 0.0;
        var bestMagnitude = SnapThreshold;
        foreach (var target in targets)
        {
            var correction = SnapCorrection(target, guides);
            if (correction != 0.0 && Math.Abs(correction) < bestMagnitude)
            {
                bestMagnitude = Math.Abs(correction);
                best = correction;
            }
        }

        return best;
    }

    private static bool IsShiftDown() =>
        Microsoft.UI.Input.InputKeyboardSource
            .GetKeyStateForCurrentThread(Windows.System.VirtualKey.Shift)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

    // ---- S3 keyboard nudge ----------------------------------------------------
    private void OnEditorKeyDown(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs e)
    {
        if (_selectedLayer is null || _dragLayer is not null)
        {
            return;
        }

        var step = IsShiftDown() ? 0.02 : 0.005;
        double deltaX = 0, deltaY = 0;
        switch (e.Key)
        {
            case Windows.System.VirtualKey.Left: deltaX = -step; break;
            case Windows.System.VirtualKey.Right: deltaX = step; break;
            case Windows.System.VirtualKey.Up: deltaY = -step; break;
            case Windows.System.VirtualKey.Down: deltaY = step; break;
            default: return;
        }

        var layer = _selectedLayer;
        var x = Math.Clamp(layer.X + deltaX, 0, 1 - layer.Width);
        var y = Math.Clamp(layer.Y + deltaY, 0, 1 - layer.Height);
        layer.SetCanvasRect(x, y, layer.Width, layer.Height);
        if (_layerFrames.TryGetValue(layer.LayerIndex, out var frame))
        {
            ApplyLayerGeometry(frame, layer);
        }

        e.Handled = true;
    }

    private void OnLayerPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (sender is Border frame)
        {
            frame.ReleasePointerCapture(e.Pointer);
        }

        if (_dragLayer is not null && _dragMoved)
        {
            _dragLayer.ApplyRoute();
            LayerChanged?.Invoke(this, _dragLayer);
        }

        _dragLayer = null;
        _dragMoved = false;
        _dragStartRect = null;
        _dragStartSourceOffsetX = 0;
        _dragStartSourceOffsetY = 0;
        InteractionChanged?.Invoke(this, false);
        e.Handled = true;
    }

    private static SolidColorBrush BrushForBorderStyle(string style, string color) =>
        style switch
        {
            "program" => new SolidColorBrush(Color.FromArgb(255, 240, 168, 92)),
            "warning" => new SolidColorBrush(Color.FromArgb(255, 224, 90, 90)),
            "none" => new SolidColorBrush(Color.FromArgb(0, 0, 0, 0)),
            _ => BrushFromHex(style == "solid" ? color : "#44C1A1")
        };

    private static SolidColorBrush BrushFromHex(string hex)
    {
        hex = (hex ?? "#44C1A1").Trim().TrimStart('#');
        if (hex.Length == 6)
        {
            hex = "FF" + hex;
        }

        if (hex.Length != 8 || !uint.TryParse(hex, System.Globalization.NumberStyles.HexNumber, null, out var value))
        {
            value = 0xFF44C1A1;
        }

        return new SolidColorBrush(Color.FromArgb(
            (byte)((value >> 24) & 0xFF),
            (byte)((value >> 16) & 0xFF),
            (byte)((value >> 8) & 0xFF),
            (byte)(value & 0xFF)));
    }
}
