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
            edge ? null : distance?.GetValue<double>() ?? defaults.Distance, edge,
            type == "reveal" ? motion["feather"]?.GetValue<double>() ?? defaults.Feather : null);
    }

    /// <summary>A layer the native core gives its own motion unit: its own moves, or a loop (P7).</summary>
    private static bool IsMotionUnit(JsonObject node) => node["animations"] is JsonObject || node["loop"] is JsonObject;

    /// <summary>The most pieces a staggered text layer can split into (native GraphicsTemplate's bound).</summary>
    private static int StaggerPieces(JsonObject node, string by) =>
        by == "word" ? GraphicsMotionModel.StaggerMaxWords
            : node["maxLines"] is JsonValue lines && lines.TryGetValue<double>(out var count) ? (int)count : 1;

    private static double StaggerSpread(JsonObject node) =>
        LayerStaggerOf(node) is { } stagger ? (StaggerPieces(node, stagger.By) - 1) * stagger.Ms : 0;

    private static GraphicsLayerStagger? LayerStaggerOf(JsonObject node) =>
        node["stagger"] is JsonObject stagger && stagger["by"]?.GetValue<string>() is { } by
            ? new(by, stagger["ms"]?.GetValue<double>() ?? 0) : null;

    public static GraphicsLayerStagger? LayerStagger(NativeGraphicsScratch scratch, string id) =>
        Walk(Read(scratch)["layers"]!.AsArray()).FirstOrDefault(row => row.Node["id"]?.GetValue<string>() == id).Node is { } node
            ? LayerStaggerOf(node) : null;

    public static NativeGraphicsScratch SetLayerStagger(NativeGraphicsScratch scratch, string id, GraphicsLayerStagger? stagger)
    {
        var root = Read(scratch);
        var node = Walk(root["layers"]!.AsArray()).FirstOrDefault(row => row.Node["id"]?.GetValue<string>() == id).Node
            ?? throw new InvalidDataException("Layer was removed.");
        if (stagger is null)
        {
            if (node["stagger"] is null) return scratch;
            node.Remove("stagger");
            if (Mirrors(root)) MirrorOut(root);
            return Save(root);
        }
        if (node[LayerTypeKey]?.GetValue<string>() != "text") throw new InvalidDataException("Only text layers can build line by line or word by word.");
        if (stagger.By is not ("line" or "word")) throw new InvalidDataException("Choose line by line or word by word.");
        if (!double.IsFinite(stagger.Ms) || stagger.Ms < 10 || stagger.Ms > 1000) throw new InvalidDataException("Choose a gap from the list.");
        if (node["animations"] is not JsonObject) throw new InvalidOperationException("Give this layer its own motion first.");
        if (stagger.By == "word" && node["overflow"]?.GetValue<string>() == "wrap-then-ellipsis")
            throw new InvalidDataException("Wrapping text builds line by line.");
        var spread = (StaggerPieces(node, stagger.By) - 1) * stagger.Ms;
        // A mirrored Out is recomputed below to fit, so only In is checked then.
        foreach (var phase in Mirrors(root) ? new[] { "in" } : new[] { "in", "out" })
            if (ReadMotion(node, phase) is { } motion && motion.StartMs + motion.DurationMs + spread > 3000)
                throw new InvalidDataException(StaggerTooLong(stagger.By));
        node["stagger"] = new JsonObject { ["by"] = stagger.By, ["ms"] = stagger.Ms };
        if (Mirrors(root)) MirrorOut(root);
        return Save(root);
    }

    private static string StaggerTooLong(string by) =>
        $"With its stagger, the last {(by == "word" ? "word (up to 12)" : "line")} must finish within 3.0 s. Shorten the gap or the move.";

    public static GraphicsLayerLoop? LayerLoop(NativeGraphicsScratch scratch, string id)
    {
        var node = Walk(Read(scratch)["layers"]!.AsArray()).FirstOrDefault(row => row.Node["id"]?.GetValue<string>() == id).Node;
        if (node?["loop"] is not JsonObject loop) return null;
        return new(loop[LayerTypeKey]?.GetValue<string>() ?? "", loop["periodMs"]?.GetValue<double>() ?? 0,
            loop["durationMs"]?.GetValue<double>(), loop["direction"]?.GetValue<string>(), loop["distance"]?.GetValue<double>());
    }

    public static NativeGraphicsScratch SetLayerLoop(NativeGraphicsScratch scratch, string id, GraphicsLayerLoop? loop)
    {
        var root = Read(scratch);
        var rows = Walk(root["layers"]!.AsArray()).ToArray();
        var node = rows.FirstOrDefault(row => row.Node["id"]?.GetValue<string>() == id).Node
            ?? throw new InvalidDataException("Layer was removed.");
        if (loop is null)
        {
            if (node["loop"] is null) return scratch;
            node.Remove("loop");
            return Save(root);
        }
        _ = GraphicsMotionModel.DefaultLoop(loop.Type);
        if (!double.IsFinite(loop.PeriodMs) || loop.PeriodMs < 250 || loop.PeriodMs > 60000)
            throw new InvalidDataException("A loop repeats every 0.25 to 60 s.");
        if (loop.DurationMs is { } duration && (loop.Type != "sweep" || !double.IsFinite(duration) || duration < 50 || duration > loop.PeriodMs))
            throw new InvalidDataException("A sweep crosses within its period.");
        if (loop.Direction is { } direction && (loop.Type == "pulse" || direction is not ("left" or "right" or "up" or "down")))
            throw new InvalidDataException("Choose a direction from the list.");
        if (loop.Distance is { } distance && (loop.Type != "drift" || !double.IsFinite(distance) || distance < 1 || distance > 256))
            throw new InvalidDataException("Drift moves 1 to 256 px.");
        if (!IsMotionUnit(node) && rows.Count(row => IsMotionUnit(row.Node)) >= MotionLayerLimit)
            throw new InvalidOperationException(MotionLimitMessage);
        var value = new JsonObject { [LayerTypeKey] = loop.Type, ["periodMs"] = loop.PeriodMs };
        if (loop.Type == "sweep") value["durationMs"] = loop.DurationMs ?? Math.Min(1000, loop.PeriodMs);
        if (loop.Type is "sweep" or "drift") value["direction"] = loop.Direction ?? (loop.Type == "drift" ? "up" : "left");
        if (loop.Type == "drift") value["distance"] = loop.Distance ?? 6;
        node["loop"] = value;
        return Save(root);
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
            if (!IsMotionUnit(node) && rows.Count(row => IsMotionUnit(row.Node)) >= MotionLayerLimit)
                throw new InvalidOperationException(MotionLimitMessage);
            if (LayerStaggerOf(node) is { } stagger && motion.StartMs + motion.DurationMs + StaggerSpread(node) > 3000)
                throw new InvalidDataException(StaggerTooLong(stagger.By));
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
        if (motion.Feather is { } feather && (motion.Type != "reveal" || !double.IsFinite(feather) || feather < 0 || feather > 512))
            throw new InvalidDataException("Choose an edge from the list.");
    }

    private static void WriteMotion(JsonObject node, string phase, GraphicsLayerMotion? motion)
    {
        var animations = node["animations"] as JsonObject;
        if (motion is null)
        {
            animations?.Remove(phase);
            if (animations?.Count == 0)
            {
                node.Remove("animations");
                node.Remove("stagger");  // a stagger times the layer's own moves; none are left
            }
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
        if (motion.Type == "reveal") value["feather"] = motion.Feather ?? defaults.Feather;
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
            // A staggered layer's pieces run spread ms past its own move; mirror
            // inside that so the last piece still leaves by the In total.
            WriteMotion(row.Node, "out", motion is null ? null : Mirror(motion, inMs - StaggerSpread(row.Node)));
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
        foreach (var row in Walk(layers))
        {
            row.Node.Remove("animations");
            row.Node.Remove("stagger");  // the preset retimes every bar; set a stagger again afterwards
        }
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
            static bool Shape(JsonObject node) => node[LayerTypeKey]?.GetValue<string>() is "rect" or "ellipse" or "line" or "image";
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
            var unit = IsMotionUnit(row.Node);
            var own = ReadMotion(row.Node, phase);
            // Native GraphicsTemplate: a loop-only unit with no moving ancestor
            // moves with the graphic, like a top-level one.
            var topLevel = row.Depth == 0 || row.Node["animations"] is null && !hasAncestor;
            if (own is not null) total = Math.Max(total, own.StartMs + own.DurationMs + StaggerSpread(row.Node));
            else if (unit ? topLevel : !hasAncestor && row.Node[LayerTypeKey]?.GetValue<string>() != "group") follows = true;
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
            if (!follows && LayerStaggerOf(row.Node) is { } stagger)
                caption += stagger.By == "word" ? " · word by word" : " · line by line";
            if (row.Node["loop"] is JsonObject loop &&
                GraphicsMotionModel.Loops.FirstOrDefault(choice => choice.Id == loop[LayerTypeKey]?.GetValue<string>()).Label is { } loopLabel)
                caption += " · loops: " + loopLabel.ToLowerInvariant();
            var label = LabelOf(row.Node);
            return new GraphicsMotionBar(row.Node["id"]!.GetValue<string>(), label, row.Depth, follows,
                motion.Type, motion.StartMs, motion.DurationMs, caption, $"{label} · {caption}");
        }).ToArray();
    }
}
