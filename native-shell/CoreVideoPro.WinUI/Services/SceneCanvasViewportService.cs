namespace CoreVideoPro.WinUI.Services;

public sealed record SceneCanvasViewportSize(double Width, double Height);

public static class SceneCanvasViewportService
{
    public const double DefaultEditorMaxHeight = 360;

    public static SceneCanvasViewportSize ResolveFitSize(
        double availableWidth,
        double availableHeight,
        double canvasAspectRatio,
        double maxHeight = 0)
    {
        if (availableWidth <= 0 || canvasAspectRatio <= 0 || !double.IsFinite(canvasAspectRatio))
        {
            return new SceneCanvasViewportSize(0, 0);
        }

        if (maxHeight > 0 && (availableHeight <= 0 || availableHeight > maxHeight))
        {
            availableHeight = maxHeight;
        }

        if (availableHeight <= 0)
        {
            return new SceneCanvasViewportSize(
                availableWidth,
                availableWidth / canvasAspectRatio);
        }

        var widthFromHeight = availableHeight * canvasAspectRatio;
        var width = Math.Min(availableWidth, widthFromHeight);
        var height = width / canvasAspectRatio;
        return new SceneCanvasViewportSize(width, height);
    }
}
