using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Layout presets seed the scene canvas; operators can drag sources freely afterward.
/// </summary>
public static class SceneCanvasLayoutService
{
    public static IReadOnlyList<RouteSelectOption> PresetOptions { get; } =
    [
        new() { Value = "single", Label = "Single" },
        new() { Value = "two-up", Label = "2-up" },
        new() { Value = "three-up", Label = "3-up" },
        new() { Value = "four-up", Label = "4-up" },
        new() { Value = "five-up", Label = "5-up" },
        new() { Value = "six-up", Label = "6-up" },
        new() { Value = "seven-up", Label = "7-up" },
        new() { Value = "eight-up", Label = "8-up" },
        new() { Value = "pip", Label = "Picture in picture" },
        new() { Value = "grid", Label = "Grid" },
        new() { Value = "speaker-slides", Label = "Speaker + slides" }
    ];

    public static void EnsureCanvasRects(IList<SourceRoute> routes, string? layoutHint = null)
    {
        if (routes.Count == 0)
        {
            return;
        }

        if (routes.All(route => route.CanvasRect is not null))
        {
            return;
        }

        ApplyPreset(ResolvePreset(layoutHint), routes);
    }

    public static void ApplyPreset(SceneCanvasPreset preset, IList<SourceRoute> routes)
    {
        var rects = BuildPresetRects(preset, routes.Count);
        for (var index = 0; index < routes.Count; index++)
        {
            routes[index].CanvasRect = rects[Math.Min(index, rects.Count - 1)].Clone();
            routes[index].FitMode = SourceRouteVisualDefaults.FitMode;
            routes[index].SourceScale = SourceRouteVisualDefaults.SourceScale;
            routes[index].SourceOffsetX = SourceRouteVisualDefaults.SourceOffsetX;
            routes[index].SourceOffsetY = SourceRouteVisualDefaults.SourceOffsetY;
            routes[index].ZIndex = index;
        }
    }

    public static void ApplyPreset(string presetWire, IList<SourceRoute> routes) =>
        ApplyPreset(PresetFromWire(presetWire), routes);

    public static SceneCanvasPreset ResolvePreset(string? layoutHint) => layoutHint switch
    {
        "two-up" => SceneCanvasPreset.TwoUp,
        "three-up" => SceneCanvasPreset.ThreeUp,
        "four-up" => SceneCanvasPreset.FourUp,
        "five-up" => SceneCanvasPreset.FiveUp,
        "six-up" => SceneCanvasPreset.SixUp,
        "seven-up" => SceneCanvasPreset.SevenUp,
        "eight-up" => SceneCanvasPreset.EightUp,
        "smart-grid" or "grid" => SceneCanvasPreset.Grid,
        "speaker-slides" => SceneCanvasPreset.SpeakerSlides,
        "host-focus" or "outro" or "single" or "full" => SceneCanvasPreset.Single,
        _ => SceneCanvasPreset.Grid
    };

    public static SceneCanvasPreset PresetFromWire(string wire) => wire switch
    {
        "single" or "full" => SceneCanvasPreset.Single,
        "two-up" => SceneCanvasPreset.TwoUp,
        "three-up" => SceneCanvasPreset.ThreeUp,
        "four-up" => SceneCanvasPreset.FourUp,
        "five-up" => SceneCanvasPreset.FiveUp,
        "six-up" => SceneCanvasPreset.SixUp,
        "seven-up" => SceneCanvasPreset.SevenUp,
        "eight-up" => SceneCanvasPreset.EightUp,
        "pip" => SceneCanvasPreset.Pip,
        "speaker-slides" => SceneCanvasPreset.SpeakerSlides,
        _ => SceneCanvasPreset.Grid
    };

    public static int? TemplateSlotCount(string presetWire) => PresetFromWire(presetWire) switch
    {
        SceneCanvasPreset.Single => 1,
        SceneCanvasPreset.TwoUp => 2,
        SceneCanvasPreset.ThreeUp => 3,
        SceneCanvasPreset.FourUp => 4,
        SceneCanvasPreset.FiveUp => 5,
        SceneCanvasPreset.SixUp => 6,
        SceneCanvasPreset.SevenUp => 7,
        SceneCanvasPreset.EightUp => 8,
        _ => null
    };

    public static IReadOnlyList<NormalizedCanvasRect> BuildPresetRects(SceneCanvasPreset preset, int layerCount)
    {
        var count = Math.Max(1, layerCount);
        return preset switch
        {
            SceneCanvasPreset.Single => [Rect(0, 0, 1, 1)],
            SceneCanvasPreset.TwoUp => PanelRects(2),
            SceneCanvasPreset.ThreeUp => PanelRects(3),
            SceneCanvasPreset.FourUp => PanelRects(4),
            SceneCanvasPreset.FiveUp => PanelRects(5),
            SceneCanvasPreset.SixUp => PanelRects(6),
            SceneCanvasPreset.SevenUp => PanelRects(7),
            SceneCanvasPreset.EightUp => PanelRects(8),
            SceneCanvasPreset.Pip => PipRects(count),
            SceneCanvasPreset.SpeakerSlides => SpeakerSlidesRects(count),
            _ => GridRects(count)
        };
    }

    private static IReadOnlyList<NormalizedCanvasRect> PanelRects(int count)
    {
        return count switch
        {
            1 => [Rect(0, 0, 1, 1)],
            2 => GridRects(2, columns: 2),
            3 => [Rect(0, 0, 0.49, 1), Rect(0.51, 0, 0.49, 0.49), Rect(0.51, 0.51, 0.49, 0.49)],
            4 => GridRects(4, columns: 2),
            5 => [
                Rect(0, 0, 0.32, 0.49), Rect(0.34, 0, 0.32, 0.49), Rect(0.68, 0, 0.32, 0.49),
                Rect(0.17, 0.51, 0.32, 0.49), Rect(0.51, 0.51, 0.32, 0.49)
            ],
            6 => GridRects(6, columns: 3),
            7 => [
                Rect(0, 0, 0.235, 0.49), Rect(0.255, 0, 0.235, 0.49), Rect(0.51, 0, 0.235, 0.49), Rect(0.765, 0, 0.235, 0.49),
                Rect(0.1275, 0.51, 0.235, 0.49), Rect(0.3825, 0.51, 0.235, 0.49), Rect(0.6375, 0.51, 0.235, 0.49)
            ],
            8 => GridRects(8, columns: 4),
            _ => GridRects(count)
        };
    }

    private static IReadOnlyList<NormalizedCanvasRect> GridRects(int count, int? columns = null)
    {
        var columnCount = columns ?? (count <= 1 ? 1 : count <= 4 ? 2 : 3);
        var rows = (int)Math.Ceiling(count / (double)columnCount);
        const double gap = 0.02;
        var cellWidth = (1 - gap * (columnCount - 1)) / columnCount;
        var cellHeight = (1 - gap * (rows - 1)) / rows;
        var rects = new List<NormalizedCanvasRect>();

        for (var index = 0; index < count; index++)
        {
            var column = index % columnCount;
            var row = index / columnCount;
            rects.Add(Rect(
                column * (cellWidth + gap),
                row * (cellHeight + gap),
                cellWidth,
                cellHeight));
        }

        return rects;
    }

    private static IReadOnlyList<NormalizedCanvasRect> PipRects(int count)
    {
        var rects = new List<NormalizedCanvasRect> { Rect(0, 0, 1, 1) };
        for (var index = 1; index < count; index++)
        {
            rects.Add(Rect(0.68, 0.68, 0.28, 0.28));
        }

        return rects;
    }

    private static IReadOnlyList<NormalizedCanvasRect> SpeakerSlidesRects(int count)
    {
        var rects = new List<NormalizedCanvasRect>
        {
            Rect(0, 0, 0.58, 1)
        };

        if (count > 1)
        {
            rects.Add(Rect(0.6, 0, 0.38, 1));
        }

        for (var index = 2; index < count; index++)
        {
            var stackIndex = index - 2;
            rects.Add(Rect(0.02, 0.08 + stackIndex * 0.22, 0.24, 0.18));
        }

        return rects;
    }

    private static NormalizedCanvasRect Rect(double x, double y, double width, double height) =>
        new()
        {
            X = x,
            Y = y,
            Width = width,
            Height = height
        };
}
