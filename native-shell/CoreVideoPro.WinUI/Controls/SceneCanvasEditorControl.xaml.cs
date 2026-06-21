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
    private NormalizedCanvasRect? _dragStartRect;
    private double _dragStartSourceOffsetX;
    private double _dragStartSourceOffsetY;

    public event EventHandler<string>? PresetRequested;
    public event EventHandler<SceneCanvasLayerViewModel>? LayerChanged;
    public event EventHandler<bool>? InteractionChanged;

    public SceneCanvasEditorControl()
    {
        InitializeComponent();
    }

    public bool IsInteracting => _dragLayer is not null;

    public void SetLayers(IReadOnlyList<SceneCanvasLayerViewModel>? layers, SceneCanvasLayerViewModel? selectedLayer)
    {
        if (_dragLayer is not null)
        {
            return;
        }

        _selectedLayer = selectedLayer;
        SyncLayers(layers ?? Array.Empty<SceneCanvasLayerViewModel>());
    }

    private void OnPresetClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button button && button.Tag is string preset)
        {
            PresetRequested?.Invoke(this, preset);
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
            SurfaceState = layer.Surface,
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

        var grip = new Border
        {
            Width = 14,
            Height = 14,
            Background = new SolidColorBrush(Color.FromArgb(220, 240, 168, 92)),
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Bottom,
            Margin = new Thickness(0, 0, 6, 6),
            CornerRadius = new CornerRadius(3),
            Tag = "resize"
        };
        content.Children.Add(grip);

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

    private static void UpdateLayerSurface(Border frame, VideoSurfaceState surface)
    {
        if (frame.Child is not Grid content)
        {
            return;
        }

        foreach (var child in content.Children)
        {
            if (child is VideoSurfaceHost host && !ReferenceEquals(host.SurfaceState, surface))
            {
                host.SurfaceState = surface;
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
                ApplySelectionStyle(frame, ReferenceEquals(layer, _selectedLayer));
            }
        }
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
                (tag == "resize" || tag == "pan-source"))
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

        var position = e.GetCurrentPoint(LayerCanvas).Position;
        var deltaX = (position.X - _dragStartPointer.X) / DesignWidth;
        var deltaY = (position.Y - _dragStartPointer.Y) / DesignHeight;

        if (_dragMode == "resize")
        {
            var width = Math.Clamp(_dragStartRect.Width + deltaX, MinLayerSize, 1 - _dragStartRect.X);
            var height = Math.Clamp(_dragStartRect.Height + deltaY, MinLayerSize, 1 - _dragStartRect.Y);
            _dragLayer.SetCanvasRect(_dragStartRect.X, _dragStartRect.Y, width, height, notify: false);
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
            _dragLayer.SetCanvasRect(x, y, _dragStartRect.Width, _dragStartRect.Height, notify: false);
        }

        ApplyLayerGeometry(frame, _dragLayer);
        e.Handled = true;
    }

    private void OnLayerPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (sender is Border frame)
        {
            frame.ReleasePointerCapture(e.Pointer);
        }

        if (_dragLayer is not null)
        {
            _dragLayer.ApplyRoute();
            LayerChanged?.Invoke(this, _dragLayer);
        }

        _dragLayer = null;
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
