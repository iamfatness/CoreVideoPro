using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace CoreVideoPro.WinUI.Controls;

// Keep the existing automation, transport and telemetry groups intact. Decide
// from measured content width, rather than the monitor size or display scale.
public sealed class StudioFooterLayoutPanel : Panel
{
    private const double Gap = 8, RowGap = 6;
    protected override Size MeasureOverride(Size availableSize)
    {
        if (Children.Count != 3) return base.MeasureOverride(availableSize);
        Children[0].Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        Children[1].Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        Children[2].Measure(new Size(availableSize.Width, double.PositiveInfinity));
        var left = Children[0].DesiredSize; var transport = Children[1].DesiredSize;
        var stacked = left.Width + Gap + transport.Width > availableSize.Width;
        var actionsHeight = stacked ? left.Height + RowGap + transport.Height : Math.Max(left.Height, transport.Height);
        return new Size(double.IsInfinity(availableSize.Width) ? left.Width + Gap + transport.Width : availableSize.Width,
            actionsHeight + RowGap + Children[2].DesiredSize.Height);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        if (Children.Count != 3) return base.ArrangeOverride(finalSize);
        var left = Children[0].DesiredSize; var transport = Children[1].DesiredSize;
        var stacked = left.Width + Gap + transport.Width > finalSize.Width;
        double telemetryTop;
        if (stacked)
        {
            Children[0].Arrange(new Rect(0, 0, finalSize.Width, left.Height));
            Children[1].Arrange(new Rect(Math.Max(0, (finalSize.Width - transport.Width) / 2), left.Height + RowGap, transport.Width, transport.Height));
            telemetryTop = left.Height + RowGap + transport.Height + RowGap;
        }
        else
        {
            var height = Math.Max(left.Height, transport.Height);
            Children[0].Arrange(new Rect(0, 0, left.Width, height));
            var remaining = finalSize.Width - left.Width - Gap;
            Children[1].Arrange(new Rect(left.Width + Gap + (remaining - transport.Width) / 2, 0, transport.Width, height));
            telemetryTop = height + RowGap;
        }
        Children[2].Arrange(new Rect(0, telemetryTop, finalSize.Width, Children[2].DesiredSize.Height));
        return finalSize;
    }
}
