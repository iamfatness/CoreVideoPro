using System.Globalization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsLayerMotion(string Type, double StartMs, double DurationMs,
    string Easing = "ease-out", string? Direction = null, double? Distance = null, bool FromEdge = false);

public sealed record GraphicsMotionBar(string LayerId, string Label, int Depth, bool Follows, string Type,
    double StartMs, double DurationMs, string Caption, string Tooltip);

public static class GraphicsMotionModel
{
    public static IReadOnlyList<(string Id, string Label)> Moves { get; } =
        [("cut", "Cut"), ("fade", "Fade"), ("rise", "Fade + rise"), ("slide", "Slide"),
         ("wipe", "Wipe"), ("grow", "Grow"), ("pop", "Pop")];
    public static IReadOnlyList<(string Id, string Label)> Feels { get; } =
        [("ease-out", "Ease out"), ("ease-in", "Ease in"), ("ease-in-out", "Smooth"), ("linear", "Linear")];
    public static IReadOnlyList<(string Id, string Label)> Presets { get; } =
        [("build-left-to-right", "Build left to right"), ("everything-fades", "Everything fades"),
         ("bar-first", "Bar first, words after")];

    public static IReadOnlyList<(string Id, string Label)> Directions(string type) => type switch
    {
        "grow" => [("down", "Up"), ("up", "Down"), ("left", "Right"), ("right", "Left")],
        "slide" or "wipe" => [("left", "From left"), ("right", "From right"), ("up", "From top"), ("down", "From bottom")],
        _ => []
    };

    public static IReadOnlyList<(string Id, string Label)> Distances(string type) => type switch
    {
        "rise" => [("12", "12 px"), ("24", "24 px"), ("48", "48 px"), ("edge", "From below the plate")],
        "slide" => [("edge", "From outside its area"), ("24", "24 px"), ("48", "48 px"), ("96", "96 px")],
        _ => []
    };

    public static GraphicsLayerMotion DefaultFor(string type) => type switch
    {
        "cut" => new(type, 0, 0),
        "fade" => new(type, 0, 250),
        "rise" => new(type, 0, 300, Distance: 24),
        "slide" => new(type, 0, 400, Direction: "left", FromEdge: true),
        "wipe" => new(type, 0, 350, Direction: "left"),
        "grow" => new(type, 0, 300, Direction: "down"),
        "pop" => new(type, 0, 350, "linear"),
        _ => throw new InvalidDataException("Choose a move from the list.")
    };

    public static string Caption(GraphicsLayerMotion motion)
    {
        if (motion.Type is "slide" or "wipe" or "grow")
        {
            var move = Moves.First(choice => choice.Id == motion.Type).Label;
            var direction = Directions(motion.Type).First(choice => choice.Id == (motion.Direction ?? DefaultFor(motion.Type).Direction)).Label;
            return move + " " + direction.ToLowerInvariant();
        }
        return Moves.First(choice => choice.Id == motion.Type).Label;
    }

    public static IReadOnlyList<GraphicsMotionBar> Bars(NativeGraphicsScratch scratch, string phase) =>
        GraphicsTemplateEditor.MotionBars(scratch, phase);

    public static double Total(NativeGraphicsScratch scratch, string phase) =>
        GraphicsTemplateEditor.MotionTotal(scratch, phase);

    public static (double StartMs, double DurationMs) Drag(double startMs, double durationMs, string handle, double deltaMs, bool fine)
    {
        var grid = fine ? 10d : 50d;
        var end = startMs + durationMs;
        double Snap(double value) => Math.Round(value / grid, MidpointRounding.AwayFromZero) * grid;
        if (durationMs == 0) return (Math.Clamp(Snap(startMs + deltaMs), 0, 3000), 0);
        var minimum = Math.Min(durationMs, 50);
        return handle switch
        {
            "body" => (Math.Clamp(Snap(startMs + deltaMs), 0, 3000 - durationMs), durationMs),
            "start" => MoveStart(Math.Clamp(Snap(startMs + deltaMs), 0, end - minimum), end),
            "end" => (startMs, Math.Clamp(Snap(end + deltaMs), startMs + minimum, 3000) - startMs),
            _ => throw new ArgumentException("Choose a motion bar handle.", nameof(handle))
        };
    }

    private static (double StartMs, double DurationMs) MoveStart(double start, double end) => (start, end - start);

    public static double ParseSeconds(string text)
    {
        var trimmed = text.Trim();
        if (trimmed.EndsWith('s')) trimmed = trimmed[..^1].TrimEnd();
        if (double.TryParse(trimmed, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out var value) && double.IsFinite(value))
            return value;
        throw new InvalidDataException("Type a time like 0.2 or 0.20 s.");
    }
}
