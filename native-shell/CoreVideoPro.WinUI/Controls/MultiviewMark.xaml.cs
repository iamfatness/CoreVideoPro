using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

/// <summary>
/// The CoreVideo Pro "Multiview" brand mark (design_handoff_corevideo_pro).
/// Pure-vector, themeable: <see cref="Stroke"/> paints the frame + cross,
/// <see cref="TileFill"/> paints the live (top-right) tile so state variants
/// (Program amber / On-air red) are a one-property change. Defaults to the
/// design live-green for both.
/// </summary>
public sealed partial class MultiviewMark : UserControl
{
    private static readonly Brush LiveGreen = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0x22, 0xC8, 0x6E));

    public MultiviewMark()
    {
        InitializeComponent();
    }

    public Brush Stroke
    {
        get => (Brush)GetValue(StrokeProperty);
        set => SetValue(StrokeProperty, value);
    }

    public static readonly DependencyProperty StrokeProperty =
        DependencyProperty.Register(nameof(Stroke), typeof(Brush), typeof(MultiviewMark),
            new PropertyMetadata(LiveGreen));

    public Brush TileFill
    {
        get => (Brush)GetValue(TileFillProperty);
        set => SetValue(TileFillProperty, value);
    }

    public static readonly DependencyProperty TileFillProperty =
        DependencyProperty.Register(nameof(TileFill), typeof(Brush), typeof(MultiviewMark),
            new PropertyMetadata(LiveGreen));
}
