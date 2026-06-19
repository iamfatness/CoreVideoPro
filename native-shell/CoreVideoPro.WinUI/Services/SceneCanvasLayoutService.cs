using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Layout presets seed the OBS-style canvas; operators can drag sources freely afterward.
/// </summary>
public static class SceneCanvasLayoutService
{
    public static IReadOnlyList<RouteSelectOption> PresetOptions { get; } =
    [
        new() { Value = "full", Label = "Full frame" },
        new() { Value = "two-up", Label = "2-up" },
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
            routes[index].ZIndex = index;
        }
    }

    public static void ApplyPreset(string presetWire, IList<SourceRoute> routes) =>
        ApplyPreset(PresetFromWire(presetWire), routes);

    public static SceneCanvasPreset ResolvePreset(string? layoutHint) => layoutHint switch
    {
        "two-up" => SceneCanvasPreset.TwoUp,
        "smart-grid" or "grid" => SceneCanvasPreset.Grid,
        "speaker-slides" => SceneCanvasPreset.SpeakerSlides,
        "host-focus" or "outro" or "full" => SceneCanvasPreset.Full,
        _ => SceneCanvasPreset.Grid
    };

    public static SceneCanvasPreset PresetFromWire(string wire) => wire switch
    {
        "full" => SceneCanvasPreset.Full,
        "two-up" => SceneCanvasPreset.TwoUp,
        "pip" => SceneCanvasPreset.Pip,
        "speaker-slides" => SceneCanvasPreset.SpeakerSlides,
        _ => SceneCanvasPreset.Grid
    };

    public static IReadOnlyList<NormalizedCanvasRect> BuildPresetRects(SceneCanvasPreset preset, int layerCount)
    {
        var count = Math.Max(1, layerCount);
        return preset switch
        {
            SceneCanvasPreset.Full => [Rect(0, 0, 1, 1)],
            SceneCanvasPreset.TwoUp => TwoColumnRects(count, gap: 0.02),
            SceneCanvasPreset.Pip => PipRects(count),
            SceneCanvasPreset.SpeakerSlides => SpeakerSlidesRects(count),
            _ => GridRects(count)
        };
    }

    private static IReadOnlyList<NormalizedCanvasRect> TwoColumnRects(int count, double gap)
    {
        var rects = new List<NormalizedCanvasRect>();
        var columns = Math.Min(2, count);
        var rows = (int)Math.Ceiling(count / (double)columns);
        var cellWidth = (1 - gap * (columns - 1)) / columns;
        var cellHeight = (1 - gap * (rows - 1)) / rows;

        for (var index = 0; index < count; index++)
        {
            var column = index % columns;
            var row = index / columns;
            rects.Add(Rect(
                column * (cellWidth + gap),
                row * (cellHeight + gap),
                cellWidth,
                cellHeight));
        }

        return rects;
    }

    private static IReadOnlyList<NormalizedCanvasRect> GridRects(int count)
    {
        var columns = count <= 1 ? 1 : count <= 4 ? 2 : 3;
        var rows = (int)Math.Ceiling(count / (double)columns);
        const double gap = 0.02;
        var cellWidth = (1 - gap * (columns - 1)) / columns;
        var cellHeight = (1 - gap * (rows - 1)) / rows;
        var rects = new List<NormalizedCanvasRect>();

        for (var index = 0; index < count; index++)
        {
            var column = index % columns;
            var row = index / columns;
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