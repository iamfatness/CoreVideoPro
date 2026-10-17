using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Private copy probes. Only fields consumed by native text layers are
/// replaced; fonts, image IDs, clocks and accepted instance values remain intact.</summary>
public static class GraphicsPreviewSamples
{
    public static JsonElement Apply(NativeGraphicsDraft draft, NativeGraphicsTemplate? template, string sample)
    {
        var values = draft.Fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone());
        var fields = template?.Fields ?? [];
        var textIds = new HashSet<string>(StringComparer.Ordinal);
        void Visit(JsonElement layers)
        {
            if (layers.ValueKind != JsonValueKind.Array) return;
            foreach (var layer in layers.EnumerateArray()) {
                if (layer.TryGetProperty("type", out var type) && type.GetString() == "text" &&
                    layer.TryGetProperty("text", out var text) && text.ValueKind == JsonValueKind.Object &&
                    text.TryGetProperty("field", out var field) && field.ValueKind == JsonValueKind.String)
                    textIds.Add(field.GetString()!);
                if (layer.TryGetProperty("children", out var children)) Visit(children);
            }
        }
        if (!draft.Declarative) {
            fields = [new() { Id = "name", Type = "string", MaxLength = 256 }, new() { Id = "title", Type = "string", MaxLength = 512 }];
            textIds.UnionWith(["name", "title"]);
        } else if (draft.Scratch is { } scratch) {
            using var document = JsonDocument.Parse(scratch.Manifest);
            if (document.RootElement.TryGetProperty("layers", out var layers)) Visit(layers);
        } else if (template?.Layers is { } layers) Visit(layers);
        else if (template?.Definition is { } definition && definition.TryGetProperty("layers", out var nodes)) Visit(nodes);
        var content = fields.Where(f => f.Type == "string" && f.Choices.Count == 0 && textIds.Contains(f.Id) && f.Id is not ("time" or "label")).ToArray();
        for (var index = 0; index < content.Length; index++) {
            var field = content[index];
            var text = sample switch {
                "short" => index == 0 ? "Ada" : "Host",
                "long" => index == 0 ? "Dr. Alexandra Montgomery-Williams — International Research Director" : "A longer secondary line to check wrapping, alignment and clipping",
                "unicode" => index == 0 ? "Zoë · José · 日本語" : "العربية · Français · Español",
                "blank" => index == 0 ? values.GetValueOrDefault(field.Id) is { ValueKind: JsonValueKind.String } current ? current.GetString()! : "Ada" : "",
                _ => values.GetValueOrDefault(field.Id) is { ValueKind: JsonValueKind.String } current ? current.GetString()! : ""
            };
            var limited = new StringBuilder(); var bytes = 0;
            foreach (var rune in text.EnumerateRunes()) {
                if (bytes + rune.Utf8SequenceLength > (field.MaxLength ?? 512)) break;
                limited.Append(rune); bytes += rune.Utf8SequenceLength;
            }
            values[field.Id] = JsonSerializer.SerializeToElement(limited.ToString());
        }
        return JsonSerializer.SerializeToElement(values);
    }
}
