using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// Drawn processor response (C6 workspace): EQ frequency response, compressor
/// transfer, or gate transfer — computed by DspCurveMath from the SAME
/// parameters the core chain applies, so the graph is the actual processing.
/// Kind: "eq" (P1=highpassHz, P2=presenceHz, P3=presenceDb),
/// "comp" (P1=thresholdDb, P2=ratio), "gate" (P1=thresholdDb).
/// </summary>
public sealed partial class DspResponseCurve : UserControl
{
    public DspResponseCurve()
    {
        InitializeComponent();
        SizeChanged += (_, _) => Redraw();
        Loaded += (_, _) => ApplyEngagedStroke();
    }

    public static readonly DependencyProperty KindProperty = DependencyProperty.Register(
        nameof(Kind), typeof(string), typeof(DspResponseCurve),
        new PropertyMetadata("eq", static (d, _) => ((DspResponseCurve)d).Redraw()));

    public static readonly DependencyProperty P1Property = DependencyProperty.Register(
        nameof(P1), typeof(double), typeof(DspResponseCurve),
        new PropertyMetadata(0.0, static (d, _) => ((DspResponseCurve)d).Redraw()));

    public static readonly DependencyProperty P2Property = DependencyProperty.Register(
        nameof(P2), typeof(double), typeof(DspResponseCurve),
        new PropertyMetadata(0.0, static (d, _) => ((DspResponseCurve)d).Redraw()));

    public static readonly DependencyProperty P3Property = DependencyProperty.Register(
        nameof(P3), typeof(double), typeof(DspResponseCurve),
        new PropertyMetadata(0.0, static (d, _) => ((DspResponseCurve)d).Redraw()));

    // C6b: 8-band gains as a csv string ("0,0,-3,…") — x:Bind-friendly single value.
    public static readonly DependencyProperty BandsProperty = DependencyProperty.Register(
        nameof(Bands), typeof(string), typeof(DspResponseCurve),
        new PropertyMetadata("", static (d, _) => ((DspResponseCurve)d).Redraw()));

    public string Bands
    {
        get => (string)GetValue(BandsProperty);
        set => SetValue(BandsProperty, value);
    }

    // C6c (owner: "graphics don't reflect their state"): engaged = bright green
    // = the core is processing; disengaged = dim grey preview of what IN would
    // do. The graph must never look live when the insert isn't on the chain.
    public static readonly DependencyProperty IsEngagedProperty = DependencyProperty.Register(
        nameof(IsEngaged), typeof(bool), typeof(DspResponseCurve),
        new PropertyMetadata(false, static (d, _) => ((DspResponseCurve)d).ApplyEngagedStroke()));

    public bool IsEngaged
    {
        get => (bool)GetValue(IsEngagedProperty);
        set => SetValue(IsEngagedProperty, value);
    }

    private static readonly Microsoft.UI.Xaml.Media.SolidColorBrush EngagedStroke =
        new(Windows.UI.Color.FromArgb(255, 63, 210, 168));

    private static readonly Microsoft.UI.Xaml.Media.SolidColorBrush BypassedStroke =
        new(Windows.UI.Color.FromArgb(255, 70, 84, 92));

    private void ApplyEngagedStroke()
    {
        CurveLine.Stroke = IsEngaged ? EngagedStroke : BypassedStroke;
        CurveLine.Opacity = IsEngaged ? 1.0 : 0.55;
    }

    public string Kind
    {
        get => (string)GetValue(KindProperty);
        set => SetValue(KindProperty, value);
    }

    public double P1
    {
        get => (double)GetValue(P1Property);
        set => SetValue(P1Property, value);
    }

    public double P2
    {
        get => (double)GetValue(P2Property);
        set => SetValue(P2Property, value);
    }

    public double P3
    {
        get => (double)GetValue(P3Property);
        set => SetValue(P3Property, value);
    }

    private static double[] ParseBands(string csv)
    {
        if (string.IsNullOrWhiteSpace(csv))
        {
            return [];
        }

        var parts = csv.Split(',');
        var bands = new double[parts.Length];
        for (var index = 0; index < parts.Length; index++)
        {
            _ = double.TryParse(parts[index], System.Globalization.CultureInfo.InvariantCulture, out bands[index]);
        }

        return bands;
    }

    private void Redraw()
    {
        var width = CurveHost.ActualWidth;
        var height = CurveHost.ActualHeight;
        if (width < 8 || height < 8)
        {
            return;
        }

        var points = Kind switch
        {
            "comp" => DspCurveMath.CompressorTransfer(P1, P2),
            "gate" => DspCurveMath.GateTransfer(P1),
            _ => DspCurveMath.EqResponse(P1, ParseBands(Bands))
        };

        CurveLine.Points.Clear();
        foreach (var (x, y) in points)
        {
            CurveLine.Points.Add(new Point(x * width, (1 - y) * height));
        }

        // Reference: unity line for transfer curves, 0 dB line for EQ.
        ReferenceLine.Points.Clear();
        if (Kind is "comp" or "gate")
        {
            ReferenceLine.Points.Add(new Point(0, height));
            ReferenceLine.Points.Add(new Point(width, 0));
        }
        else
        {
            var zeroDbY = (1 - (0 - DspCurveMath.MinDb) / (DspCurveMath.MaxDb - DspCurveMath.MinDb)) * height;
            ReferenceLine.Points.Add(new Point(0, zeroDbY));
            ReferenceLine.Points.Add(new Point(width, zeroDbY));
        }
    }
}
