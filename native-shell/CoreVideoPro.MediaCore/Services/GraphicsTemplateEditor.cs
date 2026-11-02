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
        KeepRequiredFieldsUsed(Read(scratch),root); return Save(root);
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
        KeepRequiredFieldsUsed(Read(scratch),root); return Save(root);
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

    // ---- Variables (template fields). Limits mirror native GraphicsTemplate.cpp validation. ----
    private const int MaxFields=32;
    private static readonly System.Text.RegularExpressions.Regex FieldIdPattern=new(@"\A[a-z][a-z0-9-]{0,63}\z",System.Text.RegularExpressions.RegexOptions.CultureInvariant);
    private static readonly System.Text.RegularExpressions.Regex ColourPattern=new(@"\A#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})\z",System.Text.RegularExpressions.RegexOptions.CultureInvariant);
    private static string NativeFieldType(string kind)=>kind switch {
        "text"=>"string","number"=>"number","colour"=>"color","on-off"=>"boolean","image"=>"image",
        _=>throw new InvalidDataException("Choose a variable kind: text, number, colour, on-off or image.") };
    // Layer property -> native field type it may be bound to (native resolve() rejects a mismatch).
    private static string? BindableType(string property)=>property switch {
        "text"=>"string","color" or "fill"=>"color","asset"=>"image",
        "opacity" or "fontSize" or "cornerRadius"=>"number",_=>null };
    // Native quantity() ranges for the number-bindable properties.
    private static (double Min,double Max) NumberRange(string property)=>property switch { "fontSize"=>(1,512),"cornerRadius"=>(0,4096),_=>(0,1) };
    // Properties each native layer type accepts (GraphicsTemplate.cpp keys(node, allowed)).
    private static bool LayerAllows(string layerType,string property)=>property is "x" or "y" or "width" or "height" or "opacity" or "visible" || layerType switch {
        "rect"=>property is "fill" or "cornerRadius",
        "text"=>property is "text" or "font" or "fontWeight" or "fontSize" or "lineHeight" or "color" or "align" or "maxLines" or "overflow",
        "image"=>property is "asset" or "fit" or "align",
        "group"=>property is "flow" or "gap" or "padding",
        _=>false };
    // Field references may nest (gradient stops), so look through objects and arrays.
    private static bool Contains(JsonNode? value,string id)=>value switch {
        JsonObject o=>(o["field"] is JsonValue f && f.TryGetValue<string>(out var name) && name==id) || o.Any(p=>Contains(p.Value,id)),
        JsonArray a=>a.Any(i=>Contains(i,id)),_=>false };
    private static void RewriteReferences(JsonNode? value,string id,string newId)
    {
        if(value is JsonObject o) {
            if(o["field"] is JsonValue f && f.TryGetValue<string>(out var name) && name==id) o["field"]=newId;
            foreach(var child in o.Select(p=>p.Value).ToArray()) RewriteReferences(child,id,newId);
        } else if(value is JsonArray a) foreach(var child in a.ToArray()) RewriteReferences(child,id,newId);
    }
    private static IEnumerable<string> UsedProperties(JsonObject layer,string id)=>layer.Where(p=>p.Key is not ("metadata" or "children") && Contains(p.Value,id)).Select(p=>p.Key);
    private static HashSet<string> UsedFieldIds(JsonObject root)
    {
        var used=new HashSet<string>(StringComparer.Ordinal);
        foreach(var id in Fields(root).OfType<JsonObject>().Select(f=>f["id"]!.GetValue<string>()))
            if(Walk(root["layers"]!.AsArray()).Any(r=>UsedProperties(r.Node,id).Any())) used.Add(id);
        return used;
    }
    // Native rejects a template whose required field no layer uses; refuse the edit that would orphan one.
    private static void KeepRequiredFieldsUsed(JsonObject before,JsonObject after)
    {
        var usedBefore=UsedFieldIds(before); var usedAfter=UsedFieldIds(after);
        var orphaned=Fields(after).OfType<JsonObject>().Where(f=>f["required"] is JsonValue r && r.TryGetValue<bool>(out var on) && on)
            .Select(f=>f["id"]!.GetValue<string>()).Where(id=>usedBefore.Contains(id) && !usedAfter.Contains(id)).ToArray();
        if(orphaned.Length!=0) throw new InvalidOperationException("Required variable "+string.Join(", ",orphaned)+" must stay on a layer.");
    }
    private static JsonArray Fields(JsonObject root)=>root["fields"] as JsonArray ?? throw new InvalidDataException("Missing fields.");
    private static JsonObject? FindField(JsonObject root,string id)=>Fields(root).OfType<JsonObject>().FirstOrDefault(f=>f["id"]?.GetValue<string>()==id);
    private static string LabelOf(JsonObject node)=>node["metadata"] is JsonObject m && m["label"] is JsonValue l && l.TryGetValue<string>(out var caption) ? caption : node["id"]!.GetValue<string>();
    private static JsonNode DefaultFor(string type)=>type switch { "number"=>0,"color"=>"#FFFFFF","boolean"=>false,_=>"" };
    private static void CheckValue(string type,JsonNode? value,int? maxLength)
    {
        switch(type) {
            case "string" or "color" or "image":
                if(value is not JsonValue v || !v.TryGetValue<string>(out var text)) throw new InvalidDataException("Sample value must be text.");
                if(maxLength is int cap && text.Length>cap) throw new InvalidDataException($"Sample value is longer than the {cap}-character limit.");
                if(type=="color" && !ColourPattern.IsMatch(text)) throw new InvalidDataException("Use a colour like #RRGGBB.");
                if(type=="image" && text.Length!=0 && !FieldIdPattern.IsMatch(text)) throw new InvalidDataException("Choose an image asset.");
                break;
            case "number":
                if(value is not JsonValue n || JsonSerializer.SerializeToElement(n) is not { ValueKind: JsonValueKind.Number } num || !double.IsFinite(num.GetDouble())) throw new InvalidDataException("Sample value must be a number.");
                break;
            default:
                if(value is not JsonValue b || !b.TryGetValue<bool>(out _)) throw new InvalidDataException("Sample value must be on or off.");
                break;
        }
    }
    private static void CheckLength(string type,int? maxLength)
    {
        if(maxLength is null) return;
        if(type is not ("string" or "color" or "image")) throw new InvalidDataException("Only text variables have a length limit.");
        if(maxLength<0 || maxLength>4096) throw new InvalidDataException("Use a length limit of 0–4096.");
    }
    public static NativeGraphicsScratch AddField(NativeGraphicsScratch scratch,string id,string kind,int? maxLength=null)
    {
        var type=NativeFieldType(kind); var root=Read(scratch); var fields=Fields(root);
        if(!FieldIdPattern.IsMatch(id ?? "")) throw new InvalidDataException("Use a variable name of lowercase letters, digits and hyphens, starting with a letter (up to 64).");
        if(fields.Count>=MaxFields) throw new InvalidOperationException("A template supports at most 32 variables.");
        if(FindField(root,id!) is not null) throw new InvalidOperationException("A variable with that name already exists.");
        CheckLength(type,maxLength);
        var field=new JsonObject { ["id"]=id,["type"]=type,["default"]=DefaultFor(type),["bindable"]=true };
        if(maxLength is not null) field["maxLength"]=maxLength;
        CheckValue(type,field["default"],maxLength);
        fields.Add(field); return Save(root);
    }
    public static NativeGraphicsScratch RenameField(NativeGraphicsScratch scratch,string id,string newId)
    {
        var root=Read(scratch); var field=FindField(root,id) ?? throw new InvalidDataException("Variable was removed.");
        if(id==newId) return scratch;
        if(!FieldIdPattern.IsMatch(newId ?? "")) throw new InvalidDataException("Use a variable name of lowercase letters, digits and hyphens, starting with a letter (up to 64).");
        if(FindField(root,newId!) is not null) throw new InvalidOperationException("A variable with that name already exists.");
        field["id"]=newId;
        // Identifier rewrite is allowed on locked layers: leaving a stale binding would break the template.
        foreach(var row in Walk(root["layers"]!.AsArray()))
            foreach(var property in row.Node.Where(p=>p.Key is not ("metadata" or "children")).Select(p=>p.Value).ToArray()) RewriteReferences(property,id,newId);
        return Save(root);
    }
    public static IReadOnlyList<(string FieldId,IReadOnlyList<string> LayerLabels)> FieldUsage(NativeGraphicsScratch scratch)
    {
        var root=Read(scratch); var rows=Walk(root["layers"]!.AsArray()).ToArray();
        return Fields(root).OfType<JsonObject>().Select(f=>f["id"]!.GetValue<string>()).Select(id=>
            (id,(IReadOnlyList<string>)rows.Where(r=>UsedProperties(r.Node,id).Any()).Select(r=>LabelOf(r.Node)).ToArray())).ToArray();
    }
    public static NativeGraphicsScratch RemoveField(NativeGraphicsScratch scratch,string id)
    {
        var root=Read(scratch); var field=FindField(root,id) ?? throw new InvalidDataException("Variable was removed.");
        var used=FieldUsage(scratch).First(u=>u.FieldId==id).LayerLabels;
        if(used.Count!=0) throw new InvalidOperationException("Used by: "+string.Join(", ",used));
        Fields(root).Remove(field); return Save(root);
    }
    public static NativeGraphicsScratch SetFieldOptions(NativeGraphicsScratch scratch,string id,int? maxLength,JsonElement? sample,bool hideWhenEmpty)
    {
        var root=Read(scratch); var field=FindField(root,id) ?? throw new InvalidDataException("Variable was removed.");
        var type=field["type"]!.GetValue<string>(); CheckLength(type,maxLength);
        if(maxLength is not null) field["maxLength"]=maxLength; else field.Remove("maxLength");
        var cap=field["maxLength"]?.GetValue<int>();
        if(sample is JsonElement value) { var node=JsonNode.Parse(value.GetRawText()); CheckValue(type,node,cap); field["default"]=node; }
        else CheckValue(type,field["default"],cap);
        if(type=="number") {
            // Native validates with field defaults, so the default must satisfy every bound property's range.
            var number=field["default"]!.GetValue<double>();
            foreach(var row in Walk(root["layers"]!.AsArray()))
                foreach(var property in UsedProperties(row.Node,id).Where(p=>BindableType(p)=="number")) {
                    var (min,max)=NumberRange(property);
                    if(number<min || number>max) throw new InvalidDataException($"Sample value must be {min}–{max} while {property} uses this variable.");
                }
        }
        foreach(var row in Walk(root["layers"]!.AsArray()).Where(r=>r.Node["type"]!.GetValue<string>() is "text" or "image" && !r.Locked && UsedProperties(r.Node,id).Any())) {
            if(hideWhenEmpty) row.Node["collapseWhenEmpty"]=true; else row.Node.Remove("collapseWhenEmpty");
        }
        return Save(root);
    }
    public static NativeGraphicsScratch BindLayer(NativeGraphicsScratch scratch,string layerId,string property,string? fieldId,JsonElement? literal)
    {
        if((fieldId is null)==(literal is null)) throw new ArgumentException("Give either a variable or a fixed value, not both.");
        var root=Read(scratch); var layer=Walk(root["layers"]!.AsArray()).FirstOrDefault(r=>r.Node["id"]?.GetValue<string>()==layerId);
        if(layer.Node is null) throw new InvalidDataException("Layer was removed.");
        if(!LayerAllows(layer.Node["type"]!.GetValue<string>(),property)) throw new InvalidDataException("This layer does not have that property.");
        if(fieldId is null) {
            if(literal!.Value.ValueKind==JsonValueKind.Object && literal.Value.TryGetProperty("field",out _)) throw new InvalidDataException("Use a variable binding, not a fixed value, to reference a variable.");
            return Set(scratch,layerId,property,literal.Value);
        }
        var field=FindField(root,fieldId) ?? throw new InvalidDataException("Variable was removed.");
        var expected=BindableType(property) ?? throw new InvalidDataException("This layer property cannot use a variable.");
        if(field["type"]!.GetValue<string>()!=expected) throw new InvalidDataException($"This property needs a {expected} variable.");
        if(expected=="number") {
            var (min,max)=NumberRange(property); var sample=field["default"]!.GetValue<double>();
            if(sample<min || sample>max) throw new InvalidDataException($"Set the variable's sample value to {min}–{max} before using it for {property}.");
        }
        return Set(scratch,layerId,property,JsonSerializer.SerializeToElement(new JsonObject { ["field"]=fieldId }));
    }
}
