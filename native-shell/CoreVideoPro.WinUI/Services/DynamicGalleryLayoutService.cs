using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Deterministic CoreVideo Tiles layout in normalized canvas space.</summary>
public static class DynamicGalleryLayoutService
{
    public static IReadOnlyList<NormalizedCanvasRect> BuildRects(
        int tileCount,
        double canvasAspectRatio = 16.0 / 9.0,
        string tileAspectPreset = "16:9",
        double customAspectRatio = 16.0 / 9.0,
        double gutterPercent = 0.741,
        double marginPercent = 0.741)
    {
        if (tileCount <= 0)
        {
            return [];
        }

        var canvasAspect = Math.Clamp(canvasAspectRatio, 0.25, 4);
        var tileAspect = ResolveAspectRatio(tileAspectPreset, customAspectRatio);
        var gutterY = Math.Clamp(gutterPercent / 100.0, 0, 0.1);
        var marginY = Math.Clamp(marginPercent / 100.0, 0, 0.2);
        var gutterX = gutterY / canvasAspect;
        var marginX = marginY / canvasAspect;

        Candidate? best = null;
        for (var columns = 1; columns <= tileCount; columns++)
        {
            var rows = (int)Math.Ceiling(tileCount / (double)columns);
            var availableWidth = 1 - (2 * marginX) - ((columns - 1) * gutterX);
            var availableHeight = 1 - (2 * marginY) - ((rows - 1) * gutterY);
            if (availableWidth <= 0 || availableHeight <= 0)
            {
                continue;
            }

            var width = Math.Min(
                availableWidth / columns,
                (availableHeight / rows) * tileAspect / canvasAspect);
            var height = width * canvasAspect / tileAspect;
            var candidate = new Candidate(columns, rows, width, height, width * height);
            if (best is null || candidate.Area > best.Value.Area + 0.0000001 ||
                (Math.Abs(candidate.Area - best.Value.Area) < 0.0000001 && candidate.Columns < best.Value.Columns))
            {
                best = candidate;
            }
        }

        var chosen = best ?? new Candidate(1, tileCount, 1, 1.0 / tileCount, 1.0 / tileCount);
        var gridHeight = (chosen.Rows * chosen.Height) + ((chosen.Rows - 1) * gutterY);
        var top = (1 - gridHeight) / 2;
        var result = new List<NormalizedCanvasRect>(tileCount);

        for (var row = 0; row < chosen.Rows; row++)
        {
            var rowStart = row * chosen.Columns;
            var rowCount = Math.Min(chosen.Columns, tileCount - rowStart);
            var rowWidth = (rowCount * chosen.Width) + ((rowCount - 1) * gutterX);
            var left = (1 - rowWidth) / 2;
            for (var column = 0; column < rowCount; column++)
            {
                result.Add(new NormalizedCanvasRect
                {
                    X = left + column * (chosen.Width + gutterX),
                    Y = top + row * (chosen.Height + gutterY),
                    Width = chosen.Width,
                    Height = chosen.Height
                });
            }
        }

        return result;
    }

    public static string NormalizeAspectPreset(string? value) => value switch
    {
        "4:3" or "5:4" or "1:1" or "3:4" or "9:16" or "custom" => value,
        _ => "16:9"
    };

    public static double ResolveAspectRatio(string? preset, double customAspectRatio) =>
        NormalizeAspectPreset(preset) switch
        {
            "4:3" => 4.0 / 3.0,
            "5:4" => 5.0 / 4.0,
            "1:1" => 1,
            "3:4" => 3.0 / 4.0,
            "9:16" => 9.0 / 16.0,
            "custom" => Math.Clamp(customAspectRatio, 0.25, 4),
            _ => 16.0 / 9.0
        };

    private readonly record struct Candidate(int Columns, int Rows, double Width, double Height, double Area);
}
