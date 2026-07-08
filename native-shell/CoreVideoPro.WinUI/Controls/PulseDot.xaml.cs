using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// Design-system "livepulse" status dot: pulses opacity forever so LIVE /
/// ON-AIR / REC indicators read as active. <see cref="DotFill"/> sets the
/// state color (defaults to live green); <see cref="Diameter"/> the size.
/// </summary>
public sealed partial class PulseDot : UserControl
{
    private static readonly Brush LiveGreen = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0x22, 0xC8, 0x6E));

    public PulseDot()
    {
        InitializeComponent();
    }

    public Brush DotFill
    {
        get => (Brush)GetValue(DotFillProperty);
        set => SetValue(DotFillProperty, value);
    }

    public static readonly DependencyProperty DotFillProperty =
        DependencyProperty.Register(nameof(DotFill), typeof(Brush), typeof(PulseDot),
            new PropertyMetadata(LiveGreen));

    public double Diameter
    {
        get => (double)GetValue(DiameterProperty);
        set => SetValue(DiameterProperty, value);
    }

    public static readonly DependencyProperty DiameterProperty =
        DependencyProperty.Register(nameof(Diameter), typeof(double), typeof(PulseDot),
            new PropertyMetadata(7.0));

    private void OnLoaded(object sender, RoutedEventArgs args) => Pulse.Begin();
}
