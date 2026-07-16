using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class MasteringToneCurve : UserControl
{
    public static readonly DependencyProperty HighPassHzProperty = Register(nameof(HighPassHz));
    public static readonly DependencyProperty LowPassHzProperty = Register(nameof(LowPassHz));
    public static readonly DependencyProperty LowShelfDbProperty = Register(nameof(LowShelfDb));
    public static readonly DependencyProperty PresenceDbProperty = Register(nameof(PresenceDb));
    public static readonly DependencyProperty HighShelfDbProperty = Register(nameof(HighShelfDb));

    private static readonly SolidColorBrush GridBrush =
        new(Color.FromArgb(255, 31, 48, 61));
    private static readonly SolidColorBrush ZeroBrush =
        new(Color.FromArgb(255, 63, 91, 105));
    private static readonly SolidColorBrush CurveBrush =
        new(Color.FromArgb(255, 76, 203, 224));

    public MasteringToneCurve()
    {
        InitializeComponent();
        Loaded += (_, _) => RenderCurve();
        SizeChanged += (_, _) => RenderCurve();
    }

    public double HighPassHz
    {
        get => (double)GetValue(HighPassHzProperty);
        set => SetValue(HighPassHzProperty, value);
    }

    public double LowPassHz
    {
        get => (double)GetValue(LowPassHzProperty);
        set => SetValue(LowPassHzProperty, value);
    }

    public double LowShelfDb
    {
        get => (double)GetValue(LowShelfDbProperty);
        set => SetValue(LowShelfDbProperty, value);
    }

    public double PresenceDb
    {
        get => (double)GetValue(PresenceDbProperty);
        set => SetValue(PresenceDbProperty, value);
    }

    public double HighShelfDb
    {
        get => (double)GetValue(HighShelfDbProperty);
        set => SetValue(HighShelfDbProperty, value);
    }

    private static DependencyProperty Register(string name) =>
        DependencyProperty.Register(
            name,
            typeof(double),
            typeof(MasteringToneCurve),
            new PropertyMetadata(0.0, OnCurvePropertyChanged));

    private static void OnCurvePropertyChanged(
        DependencyObject dependencyObject,
        DependencyPropertyChangedEventArgs args)
    {
        if (dependencyObject is MasteringToneCurve { IsLoaded: true } curve)
        {
            curve.RenderCurve();
        }
    }

    private void RenderCurve()
    {
        var width = ActualWidth;
        var height = ActualHeight;
        if (width < 40 || height < 40)
        {
            return;
        }

        CurveCanvas.Children.Clear();
        for (var index = 1; index < 4; index++)
        {
            AddLine(0, height * index / 4, width, height * index / 4, GridBrush, 1);
        }

        foreach (var frequency in new[] { 60d, 120d, 500d, 3000d, 10000d })
        {
            var x = FrequencyToX(frequency, width);
            AddLine(x, 0, x, height, GridBrush, 1);
        }

        AddLine(0, height / 2, width, height / 2, ZeroBrush, 1.25);

        var points = new PointCollection();
        const int pointCount = 96;
        for (var index = 0; index < pointCount; index++)
        {
            var x = width * index / (pointCount - 1d);
            var frequency = XToFrequency(x, width);
            var gain = CalculateGain(frequency);
            var y = height / 2 - gain / 18d * (height / 2 - 5);
            points.Add(new Point(x, Math.Clamp(y, 3, height - 3)));
        }

        CurveCanvas.Children.Add(new Polyline
        {
            Points = points,
            Stroke = CurveBrush,
            StrokeThickness = 2.25,
            StrokeLineJoin = PenLineJoin.Round
        });
    }

    private double CalculateGain(double frequency)
    {
        var lowWeight = 1d / (1d + Math.Pow(frequency / 120d, 2.6));
        var highWeight = 1d / (1d + Math.Pow(10000d / frequency, 2.6));
        var presenceDistance = Math.Log(frequency / 3000d) / 0.58;
        var presenceWeight = Math.Exp(-0.5 * presenceDistance * presenceDistance);
        var gain = LowShelfDb * lowWeight + PresenceDb * presenceWeight + HighShelfDb * highWeight;

        if (HighPassHz > 0 && frequency < HighPassHz)
        {
            gain -= 18d * Math.Min(1d, Math.Log(HighPassHz / frequency, 2));
        }

        if (LowPassHz > 0 && frequency > LowPassHz)
        {
            gain -= 18d * Math.Min(1d, Math.Log(frequency / LowPassHz, 2));
        }

        return Math.Clamp(gain, -18d, 18d);
    }

    private static double FrequencyToX(double frequency, double width) =>
        Math.Log10(frequency / 20d) / Math.Log10(20000d / 20d) * width;

    private static double XToFrequency(double x, double width) =>
        20d * Math.Pow(20000d / 20d, x / width);

    private void AddLine(
        double x1,
        double y1,
        double x2,
        double y2,
        Brush stroke,
        double thickness)
    {
        CurveCanvas.Children.Add(new Line
        {
            X1 = x1,
            Y1 = y1,
            X2 = x2,
            Y2 = y2,
            Stroke = stroke,
            StrokeThickness = thickness
        });
    }
}
