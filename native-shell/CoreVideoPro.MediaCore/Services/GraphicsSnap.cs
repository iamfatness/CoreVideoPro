namespace CoreVideoPro.MediaCore.Services;

public readonly record struct GraphicsRect(double X, double Y, double Width, double Height);

/// <summary>A guide line on the 1920x1080 canvas: <c>Axis</c> 'x' is a vertical line at x=Position, 'y' a horizontal one.</summary>
public sealed record GraphicsGuide(char Axis, double Position, string Kind);

/// <summary>A snapped drag result: the new x/y (or width/height) in the dragged thing's own units, and the guides it reached.</summary>
public readonly record struct GraphicsSnapResult(double X, double Y, IReadOnlyList<GraphicsGuide> Guides);

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

    /// <summary>Layer drag. <paramref name="start"/> is the layer's canvas rect when the drag began, (<paramref name="startX"/>,
    /// <paramref name="startY"/>) its own x/y then and (<paramref name="x"/>,<paramref name="y"/>) the proposed x/y, in layer units
    /// (one layer unit = <paramref name="scale"/> canvas px, the graphic's placement scale). The snap is found in canvas px and
    /// converted back; x/y stay within native's 0-8192, and a guide is only reported when the clamped layer reaches it (0.5 px).</summary>
    public static GraphicsSnapResult MoveLayer(GraphicsRect start, double startX, double startY, double x, double y, double scale, double threshold = Threshold)
    {
        var (dx, dy, guides) = Move(new(start.X + (x - startX) * scale, start.Y + (y - startY) * scale, start.Width, start.Height), threshold);
        return Land(x + dx / scale, y + dy / scale, 0, 8192, 0, 8192, scale, guides);
    }

    /// <summary>Layer corner drag: (<paramref name="width"/>,<paramref name="height"/>) is the proposed size in layer units; the
    /// rect keeps <paramref name="start"/>'s origin. Width/height stay within 1-8192.</summary>
    public static GraphicsSnapResult ResizeLayer(GraphicsRect start, double width, double height, double scale, double threshold = Threshold)
    {
        var (dw, dh, guides) = Resize(new(start.X, start.Y, width * scale, height * scale), threshold);
        return Land(width + dw / scale, height + dh / scale, 1, 8192, 1, 8192, scale, guides);
    }

    /// <summary>Whole-graphic drag: <paramref name="origin"/> is the graphic's canvas rect at the drag start, (<paramref name="placementX"/>,
    /// <paramref name="placementY"/>) its placement offset then; canvas and placement px are 1:1. The offset stays within ±3840 / ±2160.</summary>
    public static GraphicsSnapResult MoveGraphic(GraphicsRect origin, double placementX, double placementY, double dx, double dy, double threshold = Threshold)
    {
        var (sx, sy, guides) = Move(new(origin.X + dx, origin.Y + dy, origin.Width, origin.Height), threshold);
        return Land(placementX + dx + sx, placementY + dy + sy, -3840, 3840, -2160, 2160, 1, guides);
    }

    private static GraphicsSnapResult Land(double x, double y, double minX, double maxX, double minY, double maxY, double scale, IReadOnlyList<GraphicsGuide> guides)
    {
        var cx = Math.Clamp(x, minX, maxX); var cy = Math.Clamp(y, minY, maxY);
        var reached = guides.Where(g => Math.Abs(g.Axis == 'x' ? cx - x : cy - y) * scale <= 0.5).ToArray();
        return new(cx, cy, reached);
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
