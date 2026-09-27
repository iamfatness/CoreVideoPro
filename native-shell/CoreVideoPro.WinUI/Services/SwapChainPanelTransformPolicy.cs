namespace CoreVideoPro.WinUI.Services;

public readonly record struct SwapChainPanelTransform(
    float ScaleX, float ScaleY, float OffsetX, float OffsetY);

public static class SwapChainPanelTransformPolicy
{
    public static SwapChainPanelTransform Resolve(
        double panelWidth, double panelHeight, int surfaceWidth, int surfaceHeight,
        bool fillPanelForTileCrop)
    {
        if (fillPanelForTileCrop)
            return new((float)(panelWidth / surfaceWidth), (float)(panelHeight / surfaceHeight), 0, 0);

        var scale = (float)Math.Min(panelWidth / surfaceWidth, panelHeight / surfaceHeight);
        return new(scale, scale,
            (float)((panelWidth - surfaceWidth * scale) / 2.0),
            (float)((panelHeight - surfaceHeight * scale) / 2.0));
    }
}
