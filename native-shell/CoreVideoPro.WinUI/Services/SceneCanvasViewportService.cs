namespace CoreVideoPro.WinUI.Services;

public sealed record SceneCanvasViewportSize(double Width, double Height);

public static class SceneCanvasViewportService
{
    private const double MinimumUsableHeight = 315;

    public static SceneCanvasViewportSize ResolveFitSize(
        double availableWidth,
        double availableHeight,
        double canvasAspectRatio)
    {
        if (availableWidth <= 0 || canvasAspectRatio <= 0 || !double.IsFinite(canvasAspectRatio))
        {
            return new SceneCanvasViewportSize(0, 0);
        }

        if (availableHeight <= 0)
        {
            return new SceneCanvasViewportSize(
                availableWidth,
                availableWidth / canvasAspectRatio);
        }

        var usableHeight = Math.Max(availableHeight, MinimumUsableHeight);
        var widthFromHeight = usableHeight * canvasAspectRatio;
        var width = Math.Min(availableWidth, widthFromHeight);
        var height = width / canvasAspectRatio;
        return new SceneCanvasViewportSize(width, height);
    }
}
