using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class AspectRatioHost : UserControl
{
    public static readonly DependencyProperty AspectWidthProperty =
        DependencyProperty.Register(nameof(AspectWidth), typeof(double), typeof(AspectRatioHost),
            new PropertyMetadata(16.0, OnAspectChanged));

    public static readonly DependencyProperty AspectHeightProperty =
        DependencyProperty.Register(nameof(AspectHeight), typeof(double), typeof(AspectRatioHost),
            new PropertyMetadata(9.0, OnAspectChanged));

    public static readonly DependencyProperty ChildProperty =
        DependencyProperty.Register(nameof(Child), typeof(UIElement), typeof(AspectRatioHost),
            new PropertyMetadata(null));

    public AspectRatioHost()
    {
        InitializeComponent();
        Loaded += (_, _) => UpdateLayoutSize();
    }

    public double AspectWidth
    {
        get => (double)GetValue(AspectWidthProperty);
        set => SetValue(AspectWidthProperty, value);
    }

    public double AspectHeight
    {
        get => (double)GetValue(AspectHeightProperty);
        set => SetValue(AspectHeightProperty, value);
    }

    public UIElement? Child
    {
        get => (UIElement?)GetValue(ChildProperty);
        set => SetValue(ChildProperty, value);
    }

    private static void OnAspectChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args) =>
        (sender as AspectRatioHost)?.UpdateLayoutSize();

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateLayoutSize();

    private void UpdateLayoutSize()
    {
        if (ActualWidth <= 0 || AspectWidth <= 0 || AspectHeight <= 0)
        {
            return;
        }

        var ratio = AspectWidth / AspectHeight;
        var availableWidth = ActualWidth;
        var availableHeight = ActualHeight;
        var heightUnbounded = double.IsInfinity(availableHeight) || availableHeight <= 0;

        double width;
        double height;
        if (heightUnbounded || availableWidth / availableHeight > ratio)
        {
            width = availableWidth;
            height = width / ratio;
        }
        else
        {
            height = availableHeight;
            width = height * ratio;
        }

        LetterboxRoot.Width = width;
        LetterboxRoot.Height = height;

        if (heightUnbounded)
        {
            Height = height;
        }
    }
}