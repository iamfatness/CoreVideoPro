using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsEditorLayer(string Id,string Type,string Label,string Parent,int Depth,bool Locked,bool Visible);

/// <summary>Bounded immutable document edits only. Native layout/rasterization
/// validates each scratch document; this type never generates media pixels.</summary>
public static class GraphicsTemplateEditor
{
    private const string LayerTypeKey="type"; // Document node discriminator, not an IPC request type.
    private static JsonObject Read(NativeGraphicsScratch scratch) => JsonNode.Parse(scratch.Manifest,new JsonNodeOptions { PropertyNameCaseInsensitive=false },new JsonDocumentOptions { MaxDepth=32 })?.AsObject() ?? throw new InvalidDataException("Invalid template object.");
    private static NativeGraphicsScratch Save(JsonObject root)=>NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(root));
    public static NativeGraphicsScratch Rename(NativeGraphicsScratch scratch,string name)
    {
        if(string.IsNullOrWhiteSpace(name) || System.Text.Encoding.UTF8.GetByteCount(name)>128) throw new InvalidDataException("Use a template name of 1–128 UTF-8 bytes.");
        var root=Read(scratch);if(root["name"]?.GetValue<string>()==name.Trim()) return scratch;
        root["name"]=name.Trim();return Save(root);
    }
    private static IEnumerable<(JsonObject Node,JsonArray Siblings,int Index,string Parent,int Depth,bool Locked)> Walk(JsonArray layers,string parent="",int depth=0,bool locked=false)
    {
        if(depth>8) throw new InvalidDataException("Template group nesting exceeds eight levels.");
        for(var i=0;i<layers.Count;i++) {
            if(layers[i] is not JsonObject node) throw new InvalidDataException("Invalid template layer.");
            if(node["id"] is not JsonValue id || !id.TryGetValue<string>(out var identity) || string.IsNullOrEmpty(identity) || identity.Length>64 ||
                node["type"] is not JsonValue kind || !kind.TryGetValue<string>(out var type) || type is not ("rect" or "text" or "image" or "group")) throw new InvalidDataException("Invalid template layer identity or type.");
            var isLocked=locked || node["metadata"] is JsonObject meta && meta["editorLocked"] is JsonValue flag && flag.TryGetValue<bool>(out var on) && on;
            yield return (node,layers,i,parent,depth,isLocked);
            if(node["children"] is JsonArray children) foreach(var child in Walk(children,node["id"]?.GetValue<string>() ?? "",depth+1,isLocked)) yield return child;
        }
    }
    public static IReadOnlyList<GraphicsEditorLayer> Layers(NativeGraphicsScratch scratch)
    {
        var root=Read(scratch); if(root["layers"] is not JsonArray layers) throw new InvalidDataException("Missing layers.");
        var result=Walk(layers).Select(row=>new GraphicsEditorLayer(row.Node["id"]!.GetValue<string>(),row.Node["type"]!.GetValue<string>(),
            row.Node["metadata"] is JsonObject metadata && metadata["label"] is JsonValue label && label.TryGetValue<string>(out var caption) ? caption : row.Node["id"]!.GetValue<string>(),row.Parent,row.Depth,row.Locked,
            row.Node["visible"] is not JsonValue visible || !visible.TryGetValue<bool>(out var on) || on)).Take(65).ToArray();
        if(result.Length>64 || result.Select(l=>l.Id).Distinct(StringComparer.Ordinal).Count()!=result.Length) throw new InvalidDataException("A template supports at most 64 layers."); return result;
    }
    public static JsonElement Layer(NativeGraphicsScratch scratch,string id)
    {
        var root=Read(scratch); var row=Walk(root["layers"]!.AsArray()).FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        return row.Node is null ? default : JsonSerializer.SerializeToElement(row.Node);
    }
    public static NativeGraphicsScratch Set(NativeGraphicsScratch scratch,string id,string property,JsonElement value)
    {
        var root=Read(scratch); var row=Walk(root["layers"]!.AsArray()).FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        if(row.Node is null) throw new InvalidDataException("Layer was removed.");
        if(row.Locked) throw new InvalidOperationException("Unlock the layer or its parent before editing.");
        if(property is not ("x" or "y" or "width" or "height" or "opacity" or "visible" or "fill" or "cornerRadius" or "font" or "fontSize" or "fontWeight" or "lineHeight" or "color" or "text" or "align" or "maxLines" or "overflow" or "gap" or "padding" or "fit" or "flow" or "asset"))
            throw new InvalidDataException("This layer property is not editable.");
        var replacement=JsonNode.Parse(value.GetRawText()); if(JsonNode.DeepEquals(row.Node[property],replacement)) return scratch;
        row.Node[property]=replacement;
        if(replacement is JsonObject reference && reference["token"] is JsonValue token && token.TryGetValue<string>(out var name)) {
            var tokens=root["brandTokens"]?.AsArray(); if(tokens is not null && !tokens.Any(t=>t?.GetValue<string>()==name)) tokens.Add(name);
        }
        return Save(root);
    }
    public static NativeGraphicsScratch Lock(NativeGraphicsScratch scratch,string id,bool locked)
    {
        var root=Read(scratch); var row=Walk(root["layers"]!.AsArray()).FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        if(row.Node is null) throw new InvalidDataException("Layer was removed.");
        if(row.Node["metadata"] is not null and not JsonObject) throw new InvalidDataException("Layer metadata must be an object to store an editor lock.");
        var metadata=row.Node["metadata"] as JsonObject ?? new JsonObject();
        if(row.Node["metadata"] is null) row.Node["metadata"]=metadata;
        metadata["editorLocked"]=locked; return Save(root);
    }
    public static NativeGraphicsScratch Move(NativeGraphicsScratch scratch,string id,int delta)
    {
        var root=Read(scratch); var row=Walk(root["layers"]!.AsArray()).FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        if(row.Node is null || row.Locked) throw new InvalidOperationException("Unlock the layer before reordering.");
        var index=Math.Clamp(row.Index+Math.Sign(delta),0,row.Siblings.Count-1); if(index==row.Index) return scratch;
        row.Siblings.RemoveAt(row.Index); row.Siblings.Insert(index,row.Node); return Save(root);
    }
    public static NativeGraphicsScratch Remove(NativeGraphicsScratch scratch,string id)
    {
        var root=Read(scratch); var row=Walk(root["layers"]!.AsArray()).FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        if(row.Node is null || row.Locked) throw new InvalidOperationException("Unlock the layer before removing it.");
        row.Siblings.RemoveAt(row.Index); if(root["layers"]!.AsArray().Count==0) throw new InvalidOperationException("Keep at least one template layer.");
        return Save(root);
    }
    public static NativeGraphicsScratch Add(NativeGraphicsScratch scratch,string type)
    {
        var root=Read(scratch);var layers=root["layers"]!.AsArray();var count=Walk(layers).Count();
        if(type is not ("rect" or "text" or "image" or "group")) throw new InvalidDataException("Unsupported native layer type.");
        if(count+(type=="group"?2:1)>64) throw new InvalidOperationException("A template supports at most 64 layers.");
        JsonObject Text()=>new() { ["id"]="layer-"+Guid.NewGuid().ToString("N"),[LayerTypeKey]="text",["text"]="Title",["font"]="Segoe UI",["fontSize"]=42,["fontWeight"]=600,["color"]="#FFFFFF",["x"]=20,["y"]=20,["width"]=600,["height"]=100 };
        var node=type=="text"?Text():new JsonObject { ["id"]="layer-"+Guid.NewGuid().ToString("N"),["type"]=type,["x"]=0,["y"]=0,["width"]=640,["height"]=180 };
        if(type=="rect") {node["fill"]="#102030";node["cornerRadius"]=8;}
        if(type=="image") {
            var asset=root["assets"]?.AsArray().FirstOrDefault()?["id"]?.GetValue<string>();
            if(asset is null) throw new InvalidOperationException("Import a package with a verified image asset before adding an image layer.");
            node["asset"]=asset;node["fit"]="contain";
        }
        if(type=="group") {node["flow"]="none";node["children"]=new JsonArray(Text());}
        layers.Add(node);return Save(root);
    }
    public static NativeGraphicsScratch Reparent(NativeGraphicsScratch scratch,string id,string parent)
    {
        var root=Read(scratch);var layers=root["layers"]!.AsArray();var rows=Walk(layers).ToArray();var row=rows.FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        if(row.Node is null || row.Locked) throw new InvalidOperationException("Unlock the layer before moving it.");
        JsonArray destination=layers;
        if(parent.Length!=0) {
            if(parent==id || (row.Node["children"] is JsonArray own && Walk(own).Any(r=>r.Node["id"]?.GetValue<string>()==parent))) throw new InvalidOperationException("A group cannot be moved inside itself.");
            var group=rows.FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==parent);
            if(group.Node?["type"]?.GetValue<string>()!="group" || group.Locked) throw new InvalidOperationException("Choose an unlocked group.");
            destination=group.Node["children"]!.AsArray();
        }
        if(row.Siblings==destination) return scratch;
        row.Siblings.RemoveAt(row.Index);destination.Add(row.Node);
        _=Walk(layers).Count();return Save(root);
    }
    public static NativeGraphicsScratch Duplicate(NativeGraphicsScratch scratch,string id)
    {
        var root=Read(scratch); var rows=Walk(root["layers"]!.AsArray()).ToArray(); var row=rows.FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==id);
        if(row.Node is null || row.Locked) throw new InvalidOperationException("Unlock the layer before duplicating.");
        var copy=row.Node.DeepClone().AsObject(); var children=copy["children"] is JsonArray nested ? Walk(nested).Select(r=>r.Node).ToArray() : [];
        if(rows.Length+1+children.Length>64) throw new InvalidOperationException("A template supports at most 64 layers.");
        foreach(var node in children.Prepend(copy)) node["id"]="layer-"+Guid.NewGuid().ToString("N");
        row.Siblings.Insert(row.Index+1,copy); return Save(root);
    }
}
