using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static partial class GraphicsTemplateEditor
{
    private const int MotionLayerLimit = 16;
    private const string MotionLimitMessage = "Up to 16 layers can have their own motion. Let this one move with the graphic.";
    private const string MirrorKey = "motionOutMirrorsIn";

    private static void CheckPhase(string phase)
    {
        if (phase is not ("in" or "out")) throw new InvalidDataException("Choose In or Out.");
    }

    public static bool OutMirrorsIn(NativeGraphicsScratch scratch) => Mirrors(Read(scratch));

    private static bool Mirrors(JsonObject root) => root["metadata"] is not JsonObject metadata ||
        metadata[MirrorKey] is not JsonValue flag || !flag.TryGetValue<bool>(out var on) || on;

    private static void SetMirrorFlag(JsonObject root, bool on)
    {
        if (root["metadata"] is not null and not JsonObject)
            throw new InvalidDataException("Template metadata must be an object to store motion settings.");
        if (on)
        {
            (root["metadata"] as JsonObject)?.Remove(MirrorKey);
            return;
        }
        if (root["metadata"] is not JsonObject metadata)
        {
            metadata = new JsonObject();
            root["metadata"] = metadata;
        }
        metadata[MirrorKey] = false;
    }

    public static NativeGraphicsScratch SetOutMirrorsIn(NativeGraphicsScratch scratch, bool on)
    {
        var root = Read(scratch);
        if (Mirrors(root) == on) return scratch;
        SetMirrorFlag(root, on);
        if (on) MirrorOut(root);
        return Save(root);
    }

    public static GraphicsLayerMotion? LayerMotion(NativeGraphicsScratch scratch, string id, string phase)
    {
        CheckPhase(phase);
        var node = Walk(Read(scratch)["layers"]!.AsArray()).FirstOrDefault(row => row.Node["id"]?.GetValue<string>() == id).Node;
        return node is null ? null : ReadMotion(node, phase);
    }

    private static GraphicsLayerMotion? ReadMotion(JsonObject node, string phase)
    {
        if (node["animations"] is not JsonObject animations || animations[phase] is not JsonObject motion) return null;
        var type = motion[LayerTypeKey]?.GetValue<string>() ?? "";
        var defaults = GraphicsMotionModel.DefaultFor(type);
        var distance = motion["distance"] as JsonValue;
        var edge = distance is null ? defaults.FromEdge : distance.TryGetValue<string>(out var text) && text == "edge";
        return new(type, motion["startMs"]?.GetValue<double>() ?? 0, motion["durationMs"]?.GetValue<double>() ?? 0,
            motion["easing"]?.GetValue<string>() ?? "ease-out", motion["direction"]?.GetValue<string>() ?? defaults.Direction,
            edge ? null : distance?.GetValue<double>() ?? defaults.Distance, edge);
    }

    public static NativeGraphicsScratch SetLayerMotion(NativeGraphicsScratch scratch, string id, string phase, GraphicsLayerMotion? motion)
    {
        CheckPhase(phase);
        var root = Read(scratch);
        if (phase == "out" && Mirrors(root)) throw new InvalidOperationException("Turn off Out mirrors In to edit the way out.");
        var rows = Walk(root["layers"]!.AsArray()).ToArray();
        var node = rows.FirstOrDefault(row => row.Node["id"]?.GetValue<string>() == id).Node
            ?? throw new InvalidDataException("Layer was removed.");
        if (motion is not null)
        {
            ValidateMotion(motion);
            if (node["animations"] is null && rows.Count(row => row.Node["animations"] is JsonObject) >= MotionLayerLimit)
                throw new InvalidOperationException(MotionLimitMessage);
        }
        else if (ReadMotion(node, phase) is null) return scratch;
        WriteMotion(node, phase, motion);
        if (Mirrors(root)) MirrorOut(root);
        return Save(root);
    }

    private static void ValidateMotion(GraphicsLayerMotion motion)
    {
        _ = GraphicsMotionModel.DefaultFor(motion.Type);
        if (!double.IsFinite(motion.StartMs) || !double.IsFinite(motion.DurationMs) ||
            motion.StartMs < 0 || motion.StartMs + motion.DurationMs > 3000)
            throw new InvalidDataException("Motion must finish within 3.0 s (starts at + takes).");
        if (motion.Type == "cut" && motion.DurationMs != 0) throw new InvalidDataException("Cut takes no time.");
        if (motion.Type != "cut" && motion.DurationMs < 50) throw new InvalidDataException("Takes must be at least 0.05 s.");
        if (!GraphicsMotionModel.Feels.Any(choice => choice.Id == motion.Easing)) throw new InvalidDataException("Choose a feel from the list.");
        var directions = GraphicsMotionModel.Directions(motion.Type);
        if (motion.Direction is { } direction && !directions.Any(choice => choice.Id == direction))
            throw new InvalidDataException("Choose a direction from the list.");
        if ((motion.FromEdge || motion.Distance is not null) && motion.Type is not ("slide" or "rise") ||
            motion.Distance is { } distance && (!double.IsFinite(distance) || distance < 1 || distance > 4096))
            throw new InvalidDataException("Choose a travel distance from the list.");
    }

    private static void WriteMotion(JsonObject node, string phase, GraphicsLayerMotion? motion)
    {
        var animations = node["animations"] as JsonObject;
        if (motion is null)
        {
            animations?.Remove(phase);
            if (animations?.Count == 0) node.Remove("animations");
            return;
        }
        if (animations is null)
        {
            animations = new JsonObject();
            node["animations"] = animations;
        }
        var value = new JsonObject { [LayerTypeKey] = motion.Type, ["startMs"] = motion.StartMs,
            ["durationMs"] = motion.DurationMs, ["easing"] = motion.Easing };
        var defaults = GraphicsMotionModel.DefaultFor(motion.Type);
        if (defaults.Direction is not null) value["direction"] = motion.Direction ?? defaults.Direction;
        if (motion.Type is "slide" or "rise")
            value["distance"] = motion.FromEdge || motion.Distance is null && defaults.FromEdge
                ? JsonValue.Create("edge") : JsonValue.Create(motion.Distance ?? defaults.Distance);
        animations[phase] = value;
    }

    public static GraphicsLayerMotion Mirror(GraphicsLayerMotion motion, double inTotalMs) =>
        motion with { StartMs = Math.Max(0, inTotalMs - (motion.StartMs + motion.DurationMs)), Easing = ReverseEase(motion.Easing) };

    private static string ReverseEase(string easing) => easing switch { "ease-in" => "ease-out", "ease-out" => "ease-in", _ => easing };

    private static void MirrorOut(JsonObject root)
    {
        var inMs = Total(root, "in");
        foreach (var row in Walk(root["layers"]!.AsArray()))
        {
            var motion = ReadMotion(row.Node, "in");
            WriteMotion(row.Node, "out", motion is null ? null : Mirror(motion, inMs));
        }
        var entrance = GraphicMotion(root).In;
        SetGraphicPhase(root, "out", entrance with { Easing = ReverseEase(entrance.Easing) });
    }

    private static void SetGraphicPhase(JsonObject root, string phase, NativeGraphicsTransition transition)
    {
        if (root["animations"] is not JsonObject animations)
        {
            animations = new JsonObject();
            root["animations"] = animations;
        }
        animations[phase] = new JsonObject { [LayerTypeKey] = transition.Type, ["direction"] = transition.Direction,
            ["durationMs"] = transition.DurationMs, ["easing"] = transition.Easing };
    }

    public static NativeGraphicsAnimations GraphicMotion(NativeGraphicsScratch scratch) => GraphicMotion(Read(scratch));

    private static NativeGraphicsAnimations GraphicMotion(JsonObject root)
    {
        var animations = root["animations"] as JsonObject;
        NativeGraphicsTransition Phase(string name)
        {
            if (animations?[name] is not JsonObject value) return new();
            return new() { Type = value[LayerTypeKey]?.GetValue<string>() ?? "none", Direction = value["direction"]?.GetValue<string>() ?? "left",
                DurationMs = value["durationMs"]?.GetValue<double>() ?? 0, Easing = value["easing"]?.GetValue<string>() ?? "linear" };
        }
        return new() { In = Phase("in"), Out = Phase("out") };
    }

    public static NativeGraphicsScratch ApplyMotionPreset(NativeGraphicsScratch scratch, string presetId)
    {
        if (!GraphicsMotionModel.Presets.Any(preset => preset.Id == presetId)) throw new InvalidDataException("Choose a motion preset.");
        var root = Read(scratch);
        var layers = root["layers"]!.AsArray();
        if (layers.Count > MotionLayerLimit) throw new InvalidOperationException(MotionLimitMessage);
        foreach (var row in Walk(layers)) row.Node.Remove("animations");
        var roots = layers.OfType<JsonObject>();
        static double Position(JsonObject node, string key) => node[key] is JsonValue scalar && scalar.TryGetValue<double>(out var value) ? value : 0;
        if (presetId == "build-left-to-right")
        {
            var index = 0;
            foreach (var node in roots.OrderBy(node => Position(node, "x")))
                WriteMotion(node, "in", new("wipe", Math.Min(index++ * 120, 2700), 300, Direction: "left"));
        }
        else if (presetId == "everything-fades")
        {
            foreach (var node in roots) WriteMotion(node, "in", GraphicsMotionModel.DefaultFor("fade"));
        }
        else
        {
            static bool Shape(JsonObject node) => node[LayerTypeKey]?.GetValue<string>() is "rect" or "image";
            foreach (var node in roots.Where(Shape)) WriteMotion(node, "in", GraphicsMotionModel.DefaultFor("wipe"));
            var index = 0;
            foreach (var node in roots.Where(node => !Shape(node)).OrderBy(node => Position(node, "y")))
                WriteMotion(node, "in", GraphicsMotionModel.DefaultFor("rise") with { StartMs = Math.Min(200 + index++ * 100, 2700) });
        }
        SetGraphicPhase(root, "in", new() { Type = "fade", DurationMs = 250, Easing = "ease-out" });
        SetMirrorFlag(root, true);
        MirrorOut(root);
        return Save(root);
    }

    private static NativeGraphicsTransition GraphicPhase(JsonObject root, string phase) =>
        phase == "in" ? GraphicMotion(root).In : GraphicMotion(root).Out;

    internal static double MotionTotal(NativeGraphicsScratch scratch, string phase)
    {
        CheckPhase(phase);
        return Total(Read(scratch), phase);
    }

    private static double Total(JsonObject root, string phase)
    {
        double total = 0;
        var follows = false;
        var animated = new HashSet<string>(StringComparer.Ordinal);
        foreach (var row in Walk(root["layers"]!.AsArray()))
        {
            var hasAncestor = animated.Contains(row.Parent);
            var unit = row.Node["animations"] is JsonObject;
            var own = ReadMotion(row.Node, phase);
            if (own is not null) total = Math.Max(total, own.StartMs + own.DurationMs);
            else if (unit ? row.Depth == 0 : !hasAncestor && row.Node[LayerTypeKey]?.GetValue<string>() != "group") follows = true;
            if (hasAncestor || unit) animated.Add(row.Node["id"]!.GetValue<string>());
        }
        var graphic = GraphicPhase(root, phase);
        return follows && graphic.Type != "none" ? Math.Max(total, graphic.DurationMs) : total;
    }

    internal static IReadOnlyList<GraphicsMotionBar> MotionBars(NativeGraphicsScratch scratch, string phase)
    {
        CheckPhase(phase);
        var root = Read(scratch);
        var graphic = GraphicPhase(root, phase);
        return Walk(root["layers"]!.AsArray()).Select(row =>
        {
            var motion = ReadMotion(row.Node, phase);
            var follows = motion is null;
            motion ??= new(graphic.Type == "none" ? "cut" : graphic.Type, 0, graphic.DurationMs, graphic.Easing,
                graphic.Type == "slide" ? graphic.Direction : null);
            var caption = GraphicsMotionModel.Caption(motion);
            if (follows) caption = "With the graphic · " + caption;
            var label = LabelOf(row.Node);
            return new GraphicsMotionBar(row.Node["id"]!.GetValue<string>(), label, row.Depth, follows,
                motion.Type, motion.StartMs, motion.DurationMs, caption, $"{label} · {caption}");
        }).ToArray();
    }
}
