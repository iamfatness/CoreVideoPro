using CoreVideoPro.WinUI.Models;
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
            : "Drag sources on the canvas. Resize from the corner grip.";

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
        var label = new TextBlock
        {
            Name = "LayerLabel",
            Text = layer.LayerLabel,
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Color.FromArgb(255, 237, 244, 239)),
            Margin = new Thickness(10, 8, 10, 0),
            VerticalAlignment = VerticalAlignment.Top
        };
        content.Children.Add(label);

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
            if (child is TextBlock textBlock && textBlock.Name == "LayerLabel")
            {
                textBlock.Text = label;
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
    }

    private void ApplySelectionStyle(Border frame, bool isSelected)
    {
        frame.BorderBrush = new SolidColorBrush(isSelected
            ? Color.FromArgb(255, 68, 193, 161)
            : Color.FromArgb(160, 68, 193, 161));
        frame.BorderThickness = new Thickness(isSelected ? 3 : 1.5);
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

        var source = e.OriginalSource as FrameworkElement;
        _dragMode = source?.Tag as string == "resize" ? "resize" : "move";
        frame.CapturePointer(e.Pointer);
        UpdateSelectionStyles();
        InteractionChanged?.Invoke(this, true);
        e.Handled = true;
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
        InteractionChanged?.Invoke(this, false);
        e.Handled = true;
    }
}