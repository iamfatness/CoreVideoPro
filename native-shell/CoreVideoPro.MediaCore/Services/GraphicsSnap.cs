namespace CoreVideoPro.MediaCore.Services;

public readonly record struct GraphicsRect(double X, double Y, double Width, double Height);

/// <summary>A guide line on the 1920x1080 canvas: <c>Axis</c> 'x' is a vertical line at x=Position, 'y' a horizontal one.</summary>
public sealed record GraphicsGuide(char Axis, double Position, string Kind);

/// <summary>Snap maths for the authoring canvas, in canvas pixels. Safe area 1728x972 at (96,54); centre lines at 960 and 540.</summary>
public static class GraphicsSnap
{
    public const double Threshold = 8;
    private const double SafeLeft = 96, SafeRight = 1824, SafeTop = 54, SafeBottom = 1026, CentreX = 960, CentreY = 540;

    public static (double Dx, double Dy, IReadOnlyList<GraphicsGuide> Guides) Move(GraphicsRect proposed, double threshold = Threshold)
    {
        var guides = new List<GraphicsGuide>(2);
        var dx = Best('x', threshold, guides,
            (proposed.X, SafeLeft, "safe"), (proposed.X + proposed.Width, SafeRight, "safe"), (proposed.X + proposed.Width / 2, CentreX, "centre"));
        var dy = Best('y', threshold, guides,
            (proposed.Y, SafeTop, "safe"), (proposed.Y + proposed.Height, SafeBottom, "safe"), (proposed.Y + proposed.Height / 2, CentreY, "centre"));
        return (dx, dy, guides);
    }

    public static (double Dw, double Dh, IReadOnlyList<GraphicsGuide> Guides) Resize(GraphicsRect proposed, double threshold = Threshold)
    {
        var guides = new List<GraphicsGuide>(2);
        var dw = Best('x', threshold, guides, (proposed.X + proposed.Width, SafeRight, "safe"), (proposed.X + proposed.Width, CentreX, "centre"));
        var dh = Best('y', threshold, guides, (proposed.Y + proposed.Height, SafeBottom, "safe"), (proposed.Y + proposed.Height, CentreY, "centre"));
        return (dw, dh, guides);
    }

    // Smallest |delta| within the threshold wins; candidates are listed safe-edge first, so ties keep the safe edge.
    private static double Best(char axis, double threshold, List<GraphicsGuide> guides, params (double Value, double Target, string Kind)[] candidates)
    {
        var found = false; var bestDelta = 0d; var bestTarget = 0d; var bestKind = "";
        foreach (var (value, target, kind) in candidates) {
            var delta = target - value;
            if (Math.Abs(delta) > threshold || (found && Math.Abs(delta) >= Math.Abs(bestDelta))) continue;
            found = true; bestDelta = delta; bestTarget = target; bestKind = kind;
        }
        if (found) guides.Add(new GraphicsGuide(axis, bestTarget, bestKind));
        return bestDelta;
    }
}
