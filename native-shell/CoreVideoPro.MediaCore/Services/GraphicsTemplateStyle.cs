using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsSavedStyle(string TemplateId, LowerThirdAppearance Appearance, JsonElement Fields);

/// <summary>Separate look from copy: name/title/clock/image references are never
/// saved or replayed as styling. Validation uses the installed field contract.</summary>
public static class GraphicsTemplateStyle
{
    public static HashSet<string> ContentFields(NativeGraphicsTemplate template)
    {
        var content=new HashSet<string>(["name","title","time","label"],StringComparer.Ordinal);
        void Visit(JsonElement value) {
            if(value.ValueKind==JsonValueKind.Object) foreach(var p in value.EnumerateObject()) {
                if(p.Name is "text" or "asset" && p.Value.ValueKind==JsonValueKind.Object && p.Value.TryGetProperty("field",out var id) && id.ValueKind==JsonValueKind.String) content.Add(id.GetString()!);
                Visit(p.Value);
            } else if(value.ValueKind==JsonValueKind.Array) foreach(var item in value.EnumerateArray()) Visit(item);
        }
        if(template.Layers is { } layers) Visit(layers);
        else if(template.Definition is { } document && document.TryGetProperty("layers",out var nodes)) Visit(nodes);
        foreach(var field in template.Fields.Where(f=>f.Type=="image")) content.Add(field.Id);
        return content;
    }
    public static GraphicsSavedStyle Capture(NativeGraphicsDraft draft,NativeGraphicsTemplate template)
    {
        var content=ContentFields(template);
        var fields=draft.Fields.EnumerateObject().Where(p=>!content.Contains(p.Name) && template.Fields.Any(f=>f.Id==p.Name)).ToDictionary(p=>p.Name,p=>p.Value.Clone());
        return new(draft.TemplateId,draft.Style,JsonSerializer.SerializeToElement(fields));
    }
    public static bool TryApply(NativeGraphicsDraft draft,NativeGraphicsTemplate template,GraphicsSavedStyle style,out NativeGraphicsDraft result)
    {
        result=draft;
        if(style.TemplateId!=draft.TemplateId || style.Fields.ValueKind!=JsonValueKind.Object || !style.Appearance.IsValid) return false;
        var content=ContentFields(template);
        var values=draft.Fields.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());
        foreach(var pair in style.Fields.EnumerateObject()) {
            var field=template.Fields.FirstOrDefault(f=>f.Id==pair.Name);
            if(content.Contains(pair.Name) || field is null || !GraphicsCommandRequest.ValidField(pair.Value,field)) return false;
            values[pair.Name]=pair.Value.Clone();
        }
        result=draft with {Style=style.Appearance,Fields=JsonSerializer.SerializeToElement(values)};return true;
    }
}
