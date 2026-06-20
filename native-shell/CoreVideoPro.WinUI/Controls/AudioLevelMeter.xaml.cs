using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class AudioLevelMeter : UserControl
{
    public static readonly DependencyProperty LevelProperty =
        DependencyProperty.Register(
            nameof(Level),
            typeof(double),
            typeof(AudioLevelMeter),
            new PropertyMetadata(0.0, OnMeterPropertyChanged));

    public static readonly DependencyProperty IsVerticalProperty =
        DependencyProperty.Register(
            nameof(IsVertical),
            typeof(bool),
            typeof(AudioLevelMeter),
            new PropertyMetadata(false, OnMeterPropertyChanged));

    public static readonly DependencyProperty SegmentCountProperty =
        DependencyProperty.Register(
            nameof(SegmentCount),
            typeof(int),
            typeof(AudioLevelMeter),
            new PropertyMetadata(18, OnMeterPropertyChanged));

    private static readonly SolidColorBrush DimBrush = new(Windows.UI.Color.FromArgb(255, 21, 30, 34));
    private static readonly SolidColorBrush GreenBrush = new(Windows.UI.Color.FromArgb(255, 46, 210, 116));
    private static readonly SolidColorBrush YellowBrush = new(Windows.UI.Color.FromArgb(255, 245, 190, 69));
    private static readonly SolidColorBrush RedBrush = new(Windows.UI.Color.FromArgb(255, 237, 76, 68));

    public AudioLevelMeter()
    {
        InitializeComponent();
        Loaded += (_, _) => RenderSegments();
    }

    public double Level
    {
        get => (double)GetValue(LevelProperty);
        set => SetValue(LevelProperty, value);
    }

    public bool IsVertical
    {
        get => (bool)GetValue(IsVerticalProperty);
        set => SetValue(IsVerticalProperty, value);
    }

    public int SegmentCount
    {
        get => (int)GetValue(SegmentCountProperty);
        set => SetValue(SegmentCountProperty, value);
    }

    private static void OnMeterPropertyChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs args)
    {
        if (dependencyObject is AudioLevelMeter meter && meter.IsLoaded)
        {
            meter.RenderSegments();
        }
    }

    private void RenderSegments()
    {
        var count = Math.Clamp(SegmentCount, 8, 48);
        var level = Math.Clamp(Level, 0, 100);
        var activeSegments = (int)Math.Round(level / 100.0 * count, MidpointRounding.AwayFromZero);

        var panel = new StackPanel
        {
            Orientation = IsVertical ? Orientation.Vertical : Orientation.Horizontal,
            HorizontalAlignment = IsVertical ? HorizontalAlignment.Center : HorizontalAlignment.Stretch,
            VerticalAlignment = IsVertical ? VerticalAlignment.Stretch : VerticalAlignment.Center,
            Spacing = 2
        };

        for (var visualIndex = 0; visualIndex < count; visualIndex++)
        {
            var lowToHighIndex = IsVertical ? count - visualIndex - 1 : visualIndex;
            var isActive = lowToHighIndex < activeSegments;
            var normalized = (lowToHighIndex + 1) / (double)count;

            panel.Children.Add(new Border
            {
                Width = IsVertical ? 14 : 4,
                Height = IsVertical ? 7 : 10,
                CornerRadius = new CornerRadius(1.5),
                Background = isActive ? BrushFor(normalized) : DimBrush
            });
        }

        RootGrid.Children.Clear();
        RootGrid.Children.Add(panel);
    }

    private static SolidColorBrush BrushFor(double normalized) =>
        normalized >= 0.9 ? RedBrush : normalized >= 0.7 ? YellowBrush : GreenBrush;
}
