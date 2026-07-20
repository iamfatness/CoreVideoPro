using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// B2 master-rack meter: a dB-scaled horizontal bar with a target/ceiling
/// guide line (master-vst-round2-spec §B2 — integrated LUFS with the
/// -14/-16/-23 target guide, true peak with the ceiling guide). Follows the
/// DspResponseCurve engage convention: bright fill while the mastering chain
/// is processing, dim grey when bypassed. With <see cref="WarnAboveGuide"/>
/// the fill turns hot when the value exceeds the guide (true peak over the
/// ceiling must read as an alarm, never as a healthy meter). Values arrive as
/// scalar snapshot props (~a few Hz); no timers, no collections.
/// </summary>
public sealed partial class MasteringGuideMeter : UserControl
{
    private static readonly SolidColorBrush EngagedFill = new(Windows.UI.Color.FromArgb(255, 63, 210, 168));
    private static readonly SolidColorBrush BypassedFill = new(Windows.UI.Color.FromArgb(255, 70, 84, 92));
    private static readonly SolidColorBrush WarnFill = new(Windows.UI.Color.FromArgb(255, 237, 76, 68));

    public MasteringGuideMeter()
    {
        InitializeComponent();
        SizeChanged += (_, _) => Redraw();
        Loaded += (_, _) => Redraw();
    }

    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(MasteringGuideMeter),
        new PropertyMetadata(-120.0, static (d, _) => ((MasteringGuideMeter)d).Redraw()));

    public static readonly DependencyProperty GuideValueProperty = DependencyProperty.Register(
        nameof(GuideValue), typeof(double), typeof(MasteringGuideMeter),
        new PropertyMetadata(-14.0, static (d, _) => ((MasteringGuideMeter)d).Redraw()));

    public static readonly DependencyProperty MinimumProperty = DependencyProperty.Register(
        nameof(Minimum), typeof(double), typeof(MasteringGuideMeter),
        new PropertyMetadata(-36.0, static (d, _) => ((MasteringGuideMeter)d).Redraw()));

    public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(
        nameof(Maximum), typeof(double), typeof(MasteringGuideMeter),
        new PropertyMetadata(0.0, static (d, _) => ((MasteringGuideMeter)d).Redraw()));

    public static readonly DependencyProperty WarnAboveGuideProperty = DependencyProperty.Register(
        nameof(WarnAboveGuide), typeof(bool), typeof(MasteringGuideMeter),
        new PropertyMetadata(false, static (d, _) => ((MasteringGuideMeter)d).Redraw()));

    public static readonly DependencyProperty IsEngagedProperty = DependencyProperty.Register(
        nameof(IsEngaged), typeof(bool), typeof(MasteringGuideMeter),
        new PropertyMetadata(true, static (d, _) => ((MasteringGuideMeter)d).Redraw()));

    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    public double GuideValue
    {
        get => (double)GetValue(GuideValueProperty);
        set => SetValue(GuideValueProperty, value);
    }

    public double Minimum
    {
        get => (double)GetValue(MinimumProperty);
        set => SetValue(MinimumProperty, value);
    }

    public double Maximum
    {
        get => (double)GetValue(MaximumProperty);
        set => SetValue(MaximumProperty, value);
    }

    public bool WarnAboveGuide
    {
        get => (bool)GetValue(WarnAboveGuideProperty);
        set => SetValue(WarnAboveGuideProperty, value);
    }

    public bool IsEngaged
    {
        get => (bool)GetValue(IsEngagedProperty);
        set => SetValue(IsEngagedProperty, value);
    }

    private void Redraw()
    {
        var width = MeterHost.ActualWidth;
        if (width < 12 || Maximum <= Minimum)
        {
            return;
        }

        var innerWidth = width - 4;  // fill sits inside the 1px border + 1px inset
        // -120 (or anything at/below the scale floor) = meter not primed: no fill.
        var fraction = Math.Clamp((Value - Minimum) / (Maximum - Minimum), 0.0, 1.0);
        FillBar.Width = Value <= Minimum ? 0 : Math.Max(0, fraction * innerWidth);
        FillBar.Fill = !IsEngaged
            ? BypassedFill
            : WarnAboveGuide && Value > GuideValue + 0.05 ? WarnFill : EngagedFill;

        var guideFraction = Math.Clamp((GuideValue - Minimum) / (Maximum - Minimum), 0.0, 1.0);
        var guideLeft = Math.Clamp(2 + guideFraction * innerWidth - 1, 0, width - 2);
        GuideLine.Margin = new Thickness(guideLeft, 1, 0, 1);
        GuideLine.Opacity = IsEngaged ? 1.0 : 0.55;
    }
}
