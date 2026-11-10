using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace CoreVideoPro.WinUI.Controls;

// Graphics, outputs, manual transport at the right edge, then full-width telemetry.
// Keep graphics and outputs together; spare space precedes the right-hand transport.
public sealed class StudioFooterLayoutPanel : Panel
{
    private const double Gap = 12, RowGap = 12;
    protected override Size MeasureOverride(Size available)
    {
        if (Children.Count != 4) return base.MeasureOverride(available);
        for (var i = 0; i < 3; i++) Children[i].Measure(new(double.PositiveInfinity, double.PositiveInfinity));
        Children[3].Measure(new(available.Width, double.PositiveInfinity));
        var g = Children[0].DesiredSize; var o = Children[1].DesiredSize; var t = Children[2].DesiredSize;
        var wrap = g.Width + t.Width + o.Width + Gap * 2 > available.Width;
        var height = wrap ? g.Height + RowGap + Math.Max(t.Height, o.Height) : Math.Max(g.Height, Math.Max(t.Height, o.Height));
        return new(double.IsInfinity(available.Width) ? g.Width + t.Width + o.Width + Gap * 2 : available.Width,
            height + RowGap + Children[3].DesiredSize.Height);
    }
    protected override Size ArrangeOverride(Size final)
    {
        if (Children.Count != 4) return base.ArrangeOverride(final);
        var g = Children[0].DesiredSize; var o = Children[1].DesiredSize; var t = Children[2].DesiredSize;
        var wrap = g.Width + t.Width + o.Width + Gap * 2 > final.Width;
        var rowHeight = Math.Max(t.Height, o.Height);
        double telemetryTop;
        if (wrap)
        {
            Children[0].Arrange(new(0, 0, g.Width, g.Height));
            var top = g.Height + RowGap;
            Children[1].Arrange(new(0, top + rowHeight - o.Height, o.Width, o.Height));
            Children[2].Arrange(new(Math.Max(o.Width + Gap, final.Width - t.Width), top + rowHeight - t.Height, t.Width, t.Height));
            telemetryTop = top + rowHeight + RowGap;
        }
        else
        {
            rowHeight = Math.Max(g.Height, rowHeight);
            Children[0].Arrange(new(0, rowHeight - g.Height, g.Width, g.Height));
            Children[1].Arrange(new(g.Width + Gap, rowHeight - o.Height, o.Width, o.Height));
            Children[2].Arrange(new(final.Width - t.Width, rowHeight - t.Height, t.Width, t.Height));
            telemetryTop = rowHeight + RowGap;
        }
        Children[3].Arrange(new(0, telemetryTop, final.Width, Children[3].DesiredSize.Height));
        return final;
    }
}
