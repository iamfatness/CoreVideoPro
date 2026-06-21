namespace CoreVideoPro.WinUI.Services;

public sealed record SourceFramingLayout(
    double Width,
    double Height,
    double TranslateX,
    double TranslateY);

public static class SourceFramingLayoutService
{
    public static SourceFramingLayout Resolve(
        double viewportWidth,
        double viewportHeight,
        double sourceWidth,
        double sourceHeight,
        string? fitMode,
        double sourceScale,
        double sourceOffsetX,
        double sourceOffsetY)
    {
        if (viewportWidth <= 0 || viewportHeight <= 0)
        {
            return new SourceFramingLayout(0, 0, 0, 0);
        }

        sourceWidth = sourceWidth > 0 ? sourceWidth : viewportWidth;
        sourceHeight = sourceHeight > 0 ? sourceHeight : viewportHeight;

        var scale = double.IsFinite(sourceScale) ? Math.Clamp(sourceScale, 0.25, 4) : 1;
        var offsetX = double.IsFinite(sourceOffsetX) ? Math.Clamp(sourceOffsetX, -1, 1) : 0;
        var offsetY = double.IsFinite(sourceOffsetY) ? Math.Clamp(sourceOffsetY, -1, 1) : 0;
        var normalizedFit = fitMode?.Trim().ToLowerInvariant();

        var sourceAspect = sourceWidth / sourceHeight;
        var viewportAspect = viewportWidth / viewportHeight;
        double baseWidth;
        double baseHeight;

        if (normalizedFit == "stretch")
        {
            baseWidth = viewportWidth;
            baseHeight = viewportHeight;
        }
        else if (normalizedFit == "fit" || normalizedFit == "contain")
        {
            if (sourceAspect > viewportAspect)
            {
                baseWidth = viewportWidth;
                baseHeight = viewportWidth / sourceAspect;
            }
            else
            {
                baseHeight = viewportHeight;
                baseWidth = viewportHeight * sourceAspect;
            }
        }
        else
        {
            if (sourceAspect > viewportAspect)
            {
                baseHeight = viewportHeight;
                baseWidth = viewportHeight * sourceAspect;
            }
            else
            {
                baseWidth = viewportWidth;
                baseHeight = viewportWidth / sourceAspect;
            }
        }

        var width = baseWidth * scale;
        var height = baseHeight * scale;
        var travelX = Math.Abs(width - viewportWidth);
        var travelY = Math.Abs(height - viewportHeight);
        var translateX = (viewportWidth - width) * 0.5 +
            (width >= viewportWidth ? -offsetX : offsetX) * travelX * 0.5;
        var translateY = (viewportHeight - height) * 0.5 +
            (height >= viewportHeight ? -offsetY : offsetY) * travelY * 0.5;

        return new SourceFramingLayout(width, height, translateX, translateY);
    }
}
