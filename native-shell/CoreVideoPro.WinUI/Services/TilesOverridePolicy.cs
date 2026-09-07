using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class TilesOverridePolicy
{
    public static TilesMemberOverride EditorValues(DynamicGallerySettings? settings, string sourceId) =>
        settings?.Overrides.TryGetValue(sourceId, out var value) == true ? value.Clone() :
            Create(0, 0, .5, 1, 0, 0, 0);
    public static TilesMemberOverride Create(double x, double y, double width, double height, double left, double right, double z)
    {
        if (!new[] { x, y, width, height, left, right, z }.All(double.IsFinite))
            throw new ArgumentException("Tiles values must be finite numbers.");
        if (x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > 1 || y + height > 1)
            throw new ArgumentException("Tile rectangle must fit inside the canvas (coordinates 0–1).");
        if (left is < 0 or > 45 || right is < 0 or > 45)
            throw new ArgumentException("Left and right crop must each be between 0 and 45 percent.");
        if (z != Math.Truncate(z) || z is < 0 or > 63)
            throw new ArgumentException("Tile order must be an integer from 0 to 63.");
        return new() { Rect = new() { X = x, Y = y, Width = width, Height = height },
            CropLeftPercent = left, CropRightPercent = right, Z = (int)z };
    }
}
