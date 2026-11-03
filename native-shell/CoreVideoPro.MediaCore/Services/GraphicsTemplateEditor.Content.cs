using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static partial class GraphicsTemplateEditor
{
    public static NativeGraphicsScratch SetStroke(NativeGraphicsScratch scratch, string id, string colour,
        double width, string? diagonal = null)
    {
        var root = Read(scratch);
        var place = EditableContent(root, id);
        var node = place.Node;
        var type = TypeOf(node);
        if (type is not ("rect" or "ellipse" or "line")) throw new InvalidDataException("Select a shape or line.");
        InRange(width, type == "line" ? 1 : 0, 4096, "Stroke width");
        if (width == 0)
        {
            node.Remove("stroke");
            node.Remove("strokeWidth");
        }
        else
        {
            node["stroke"] = ContentColour(colour);
            node["strokeWidth"] = Number(width);
        }
        if (type == "line")
        {
            if (diagonal is not ("up" or "down")) throw new InvalidDataException("Choose a line direction.");
            node["diagonal"] = diagonal;
        }
        return Commit(scratch, root);
    }

    public static NativeGraphicsScratch SetAutoWidth(NativeGraphicsScratch scratch, string id, bool enabled,
        double minimum, double maximum, double fixedWidth = 192)
    {
        var root = Read(scratch);
        var place = EditableContent(root, id);
        var node = place.Node;
        var type = TypeOf(node);
        if (type is not ("rect" or "group")) throw new InvalidDataException("Select a plate or group.");
        if (enabled)
        {
            InRange(minimum, 1, 8192, "Minimum width");
            InRange(maximum, minimum, 8192, "Maximum width");
            if (type == "rect" && place.Parent is { } parent && FlowOf(parent) == "horizontal")
                throw new InvalidOperationException("Size-to-content plates cannot sit in horizontal flow.");
            node["width"] = "auto";
            node["minWidth"] = Number(minimum);
            node["maxWidth"] = Number(maximum);
        }
        else if (node["width"] is JsonValue value && value.TryGetValue<string>(out var width) && width == "auto")
        {
            InRange(fixedWidth, 1, 8192, "Width");
            node["width"] = Number(fixedWidth);
            node.Remove("minWidth");
            node.Remove("maxWidth");
        }
        return Commit(scratch, root);
    }

    public static NativeGraphicsScratch SetTextEffects(NativeGraphicsScratch scratch, string id, double minFontSize,
        string outlineColour, double outlineWidth, string? shadowColour, double shadowX, double shadowY, double shadowBlur)
    {
        var root = Read(scratch);
        var node = EditableContent(root, id).Node;
        if (TypeOf(node) != "text") throw new InvalidDataException("Select a text layer.");
        InRange(minFontSize, 0, IsNumber(node, "fontSize") ? Literal(node, "fontSize") : 512, "Minimum font size");
        InRange(outlineWidth, 0, 8, "Outline width");
        if (minFontSize == 0) node.Remove("minFontSize");
        else
        {
            InRange(minFontSize, 1, 512, "Minimum font size");
            node["minFontSize"] = Number(minFontSize);
        }
        if (outlineWidth == 0) node.Remove("outline");
        else node["outline"] = new JsonObject { ["color"] = ContentColour(outlineColour), ["width"] = Number(outlineWidth) };
        if (shadowColour is null) node.Remove("shadow");
        else
        {
            InRange(shadowX, -256, 256, "Shadow X");
            InRange(shadowY, -256, 256, "Shadow Y");
            InRange(shadowBlur, 0, 16, "Shadow blur");
            node["shadow"] = new JsonObject { ["color"] = ContentColour(shadowColour), ["x"] = Number(shadowX),
                ["y"] = Number(shadowY), ["blur"] = Number(shadowBlur) };
        }
        return Commit(scratch, root);
    }

    private static Place EditableContent(JsonObject root, string id)
    {
        var place = Find(Places(root), id);
        if (place.Locked) throw new InvalidOperationException("Unlock the layer or its parent before editing.");
        return place;
    }

    private static void InRange(double value, double minimum, double maximum, string label)
    {
        if (!double.IsFinite(value) || value < minimum || value > maximum)
            throw new InvalidDataException($"{label} must be between {minimum:0.##} and {maximum:0.##}.");
    }

    private static string ContentColour(string value)
    {
        var colour = value.Trim();
        if (!ColourPattern.IsMatch(colour)) throw new InvalidDataException("Use a colour like #RRGGBB or #RRGGBBAA.");
        return colour;
    }
}
