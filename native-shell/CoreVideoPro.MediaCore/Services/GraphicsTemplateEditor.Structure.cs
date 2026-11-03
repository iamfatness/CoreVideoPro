using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Layer structure edits for the Designer's layers panel: rename, hide, reorder/move, group/ungroup,
/// add and duplicate. Pure document operations. Layer names and locks live under node <c>metadata</c>
/// (shell-only, native round-trips them untouched); hiding writes native <c>"visible": false</c>.
/// Positions follow native layout: a child sits at its parent's origin + padding (+ its flow step) + its own x/y.</summary>
public static partial class GraphicsTemplateEditor
{
    // Field kinds as constants: native/tests/ContractParityTest.cpp reads any C# `["type"] = "<literal>"` as a core request.
    private const string FieldKindString = "string", FieldKindImage = "image";
    private const int MaxLayers=64, MaxNesting=8, MaxLayerLabel=64;
    private sealed record Place(JsonObject Node,JsonArray Siblings,int Index,JsonObject? Parent,int Depth,bool Locked);

    private static string IdOf(JsonObject node)=>node["id"]!.GetValue<string>();
    private static string TypeOf(JsonObject node)=>node[LayerTypeKey] is JsonValue v && v.TryGetValue<string>(out var t) ? t : "";
    private static string FlowOf(JsonObject group)=>group["flow"] is JsonValue v && v.TryGetValue<string>(out var f) ? f : "none";
    private static bool IsNumber(JsonObject node,string key)=>node[key] is JsonValue v && v.GetValueKind()==JsonValueKind.Number;
    // Literal numbers only; absent or variable-bound values count as 0 (native's flow-child default).
    private static double Literal(JsonObject node,string key)=>IsNumber(node,key) ? double.Parse(node[key]!.ToJsonString(),CultureInfo.InvariantCulture) : 0;
    private static JsonNode Number(double value)
    {
        value=Math.Round(value,6); if(value==0) value=0; // no "-0"
        return value==Math.Floor(value) && Math.Abs(value)<1e15 ? JsonValue.Create((long)value) : JsonValue.Create(value);
    }
    private static Dictionary<string,Place> Places(JsonObject root)
    {
        var places=new Dictionary<string,Place>(StringComparer.Ordinal);
        foreach(var row in Walk(root["layers"] as JsonArray ?? throw new InvalidDataException("Missing layers."))) {
            var id=IdOf(row.Node); if(places.ContainsKey(id)) throw new InvalidDataException("Template layer ids must be unique.");
            places[id]=new Place(row.Node,row.Siblings,row.Index,row.Parent.Length==0 ? null : places[row.Parent].Node,row.Depth,row.Locked);
        }
        return places;
    }
    private static Place Find(Dictionary<string,Place> places,string id)=>places.TryGetValue(id ?? "",out var place) ? place : throw new InvalidDataException("Layer was removed.");
    private static string NextId(IEnumerable<string> taken,string prefix)
    {
        var used=taken.ToHashSet(StringComparer.Ordinal); var n=1; while(used.Contains($"{prefix}-{n}")) n++; return $"{prefix}-{n}";
    }
    private static JsonObject MetadataOf(JsonObject node)
    {
        if(node["metadata"] is null) node["metadata"]=new JsonObject();
        return node["metadata"] as JsonObject ?? throw new InvalidDataException("Layer metadata must be an object to store an editor name.");
    }
    // Native limits (GraphicsTemplate.cpp visit(): depth <= 8, <= 64 nodes) checked before the field walk, which throws past depth 8.
    private static void CheckStructure(JsonObject root)
    {
        int count=0,deepest=0;
        void Visit(JsonArray layers,int depth) { foreach(var node in layers.OfType<JsonObject>()) { count++; deepest=Math.Max(deepest,depth); if(node["children"] is JsonArray children) Visit(children,depth+1); } }
        Visit(root["layers"]!.AsArray(),0);
        if(deepest>MaxNesting) throw new InvalidOperationException("Template group nesting exceeds eight levels.");
        if(count>MaxLayers) throw new InvalidOperationException("A template supports at most 64 layers.");
    }
    private static NativeGraphicsScratch Commit(NativeGraphicsScratch before,JsonObject root)
    {
        CheckStructure(root); CheckPositions(root["layers"]!.AsArray()); KeepRequiredFieldsUsed(Read(before),root);
        if (Mirrors(root) && Walk(root["layers"]!.AsArray()).Any(row => row.Node["animations"] is JsonObject)) MirrorOut(root);
        return Save(root);
    }
    // Native quantity(node,"x"/"y",0,8192): never emit a literal position native would refuse to load.
    private static void CheckPositions(JsonArray layers)
    {
        foreach(var node in layers.OfType<JsonObject>()) {
            foreach(var key in new[] { "x","y" })
                if(IsNumber(node,key) && Literal(node,key) is < 0 or > 8192) throw new InvalidOperationException("This edit would place a layer outside the design area.");
            if(node["children"] is JsonArray children) CheckPositions(children);
        }
    }
    // Offset of a group's children on the canvas: the group's own canvas position plus its padding.
    private static (double X,double Y) ContentOrigin(Dictionary<string,Place> places,JsonObject? group)
    {
        if(group is null) return (0,0);
        var (x,y)=CanvasPosition(places,places[IdOf(group)]); var padding=Literal(group,"padding"); return (x+padding,y+padding);
    }
    private static (double X,double Y) FlowStep(JsonObject? parent,int index)
    {
        if(parent is null || FlowOf(parent) is not ("vertical" or "horizontal") || parent["children"] is not JsonArray children) return (0,0);
        var vertical=FlowOf(parent)=="vertical"; var gap=Literal(parent,"gap"); double step=0;
        for(var i=0;i<index && i<children.Count;i++) if(children[i] is JsonObject earlier) step+=Literal(earlier,vertical ? "height" : "width")+gap;
        return vertical ? (0,step) : (step,0);
    }
    private static (double X,double Y) CanvasPosition(Dictionary<string,Place> places,Place place)
    {
        var (ox,oy)=ContentOrigin(places,place.Parent); var (sx,sy)=FlowStep(place.Parent,place.Index);
        return (ox+sx+Literal(place.Node,"x"),oy+sy+Literal(place.Node,"y"));
    }
    private static void AddBrandTokens(JsonObject root,JsonObject node)
    {
        if(root["brandTokens"] is not JsonArray tokens) root["brandTokens"]=tokens=new JsonArray();
        foreach(var value in node.Where(p=>p.Key is not ("metadata" or "children")).Select(p=>p.Value))
            if(value is JsonObject reference && reference["token"] is JsonValue token && token.TryGetValue<string>(out var name) && !tokens.Any(t=>t?.GetValue<string>()==name)) tokens.Add(name);
    }

    /// <summary>Names the layer (<c>metadata.label</c>, 1–64 characters, trimmed). Allowed on locked layers.</summary>
    public static NativeGraphicsScratch SetLabel(NativeGraphicsScratch scratch,string id,string label)
    {
        var name=(label ?? "").Trim();
        if(name.Length is < 1 or > MaxLayerLabel) throw new InvalidDataException("Use a layer name of 1–64 characters.");
        var root=Read(scratch); var metadata=MetadataOf(Find(Places(root),id).Node);
        if(metadata["label"] is JsonValue current && current.TryGetValue<string>(out var text) && text==name) return scratch;
        metadata["label"]=name; return Commit(scratch,root);
    }
    /// <summary>Hides (<c>"visible": false</c>) or shows (removes <c>visible</c>) the layer. Allowed on locked
    /// layers; refused when a variable drives the layer's visibility.</summary>
    public static NativeGraphicsScratch SetVisible(NativeGraphicsScratch scratch,string id,bool visible)
    {
        var root=Read(scratch); var node=Find(Places(root),id).Node;
        if(node["visible"] is JsonObject bound) {
            var field=bound["field"] is JsonValue f && f.TryGetValue<string>(out var fieldId) ? fieldId : "";
            throw new InvalidOperationException($"This layer's visibility follows the variable {field}.");
        }
        if(visible) { if(node["visible"] is null) return scratch; node.Remove("visible"); }
        else { if(node["visible"] is JsonValue v && v.TryGetValue<bool>(out var on) && !on) return scratch; node["visible"]=false; }
        return Commit(scratch,root);
    }
    /// <summary>The layer's position on the design's canvas (parent origins, padding and flow steps added; a variable-bound x/y counts as 0).</summary>
    public static (double X,double Y) CanvasPositionOf(NativeGraphicsScratch scratch,string id)
    {
        var places=Places(Read(scratch)); return CanvasPosition(places,Find(places,id));
    }
    /// <summary>Moves the layer to <paramref name="parent"/> ("" = the design root) at <paramref name="frontIndex"/>
    /// (0 = frontmost; document order is back-to-front; clamped). A parent change keeps the layer where it is
    /// on the canvas (or puts it at <paramref name="absolute"/>), and a free-layout destination group grows to fit.</summary>
    public static NativeGraphicsScratch MoveTo(NativeGraphicsScratch scratch,string id,string parent,int frontIndex,(double X,double Y)? absolute=null)
    {
        var root=Read(scratch); var places=Places(root); var place=Find(places,id);
        if(place.Locked) throw new InvalidOperationException("Unlock the layer before moving it.");
        JsonObject? group=null; var destination=root["layers"]!.AsArray();
        if(!string.IsNullOrEmpty(parent)) {
            if(parent==id || (place.Node["children"] is JsonArray own && Walk(own).Any(r=>IdOf(r.Node)==parent))) throw new InvalidOperationException("A group cannot be moved inside itself.");
            if(!places.TryGetValue(parent,out var target) || TypeOf(target.Node)!="group" || target.Locked) throw new InvalidOperationException("Choose an unlocked group.");
            group=target.Node; destination=group["children"] as JsonArray ?? throw new InvalidDataException("Invalid template group.");
        }
        var sameParent=ReferenceEquals(place.Siblings,destination);
        if(!sameParent && (place.Node["x"] is JsonObject || place.Node["y"] is JsonObject))
            throw new InvalidOperationException("This layer's position follows a variable. Fix its position before moving it into another group.");
        var canvas=sameParent ? default : absolute ?? CanvasPosition(places,place);
        place.Siblings.RemoveAt(place.Index);
        // The destination's origin after the layer has left: an earlier flow sibling no longer pushes it along.
        var origin=sameParent ? default : ContentOrigin(Places(root),group);
        var at=destination.Count-Math.Clamp(frontIndex,0,destination.Count);
        if(sameParent && at==place.Index) return scratch;
        destination.Insert(at,place.Node);
        if(!sameParent) {
            if(group is not null && FlowOf(group)!="none") { place.Node["x"]=0; place.Node["y"]=0; }
            else if(group is null) { place.Node["x"]=Number(Math.Max(0,canvas.X)); place.Node["y"]=Number(Math.Max(0,canvas.Y)); }
            else Grow(group,place.Node,canvas.X-origin.X,canvas.Y-origin.Y);
        }
        return Commit(scratch,root);
    }
    // Grow a free-layout group to contain its children inside its content box (box minus padding on each side).
    // The group itself can only grow back to its parent's origin (x/y >= 0, native's range), so a moved layer
    // that sat further left/up lands at the nearest valid position, as a root move does.
    private static void Grow(JsonObject group,JsonObject moved,double movedX,double movedY)
    {
        var children=group["children"]!.AsArray().OfType<JsonObject>().ToArray();
        movedX=Math.Max(movedX,-Literal(group,"x")); movedY=Math.Max(movedY,-Literal(group,"y"));
        double X(JsonObject c)=>ReferenceEquals(c,moved) ? movedX : Literal(c,"x");
        double Y(JsonObject c)=>ReferenceEquals(c,moved) ? movedY : Literal(c,"y");
        var minX=Math.Min(0,children.Min(X)); var minY=Math.Min(0,children.Min(Y));
        if(children.Any(c=>!ReferenceEquals(c,moved) && ((minX!=0 && c["x"] is JsonObject) || (minY!=0 && c["y"] is JsonObject))))
            throw new InvalidOperationException("A layer in this group takes its position from a variable. Fix its position before moving layers in.");
        var padding=Literal(group,"padding");
        var maxX=Math.Max(Literal(group,"width")-padding*2,children.Max(c=>X(c)+Literal(c,"width")));
        var maxY=Math.Max(Literal(group,"height")-padding*2,children.Max(c=>Y(c)+Literal(c,"height")));
        if(minX!=0) group["x"]=Number(Literal(group,"x")+minX);
        if(minY!=0) group["y"]=Number(Literal(group,"y")+minY);
        if(maxX-minX+padding*2!=Literal(group,"width")) group["width"]=Number(maxX-minX+padding*2);
        if(maxY-minY+padding*2!=Literal(group,"height")) group["height"]=Number(maxY-minY+padding*2);
        foreach(var child in children) {
            var isMoved=ReferenceEquals(child,moved);
            if(isMoved || (minX!=0 && IsNumber(child,"x"))) child["x"]=Number(X(child)-minX);
            if(isMoved || (minY!=0 && IsNumber(child,"y"))) child["y"]=Number(Y(child)-minY);
        }
    }
    /// <summary>Wraps the layer in a new free-layout group (<c>group-&lt;n&gt;</c>, named "Group") that takes the
    /// layer's place and box; the layer moves to the group's origin.</summary>
    public static (NativeGraphicsScratch Scratch,string GroupId) Group(NativeGraphicsScratch scratch,string id)
    {
        var root=Read(scratch); var places=Places(root); var place=Find(places,id);
        if(place.Locked) throw new InvalidOperationException("Unlock the layer before grouping it.");
        var groupId=NextId(places.Keys,"group");
        var group=new JsonObject { ["id"]=groupId,[LayerTypeKey]="group" };
        foreach(var key in new[] { "x","y","width","height" }) if(place.Node[key] is JsonNode value) group[key]=value.DeepClone();
        group["flow"]="none"; group["metadata"]=new JsonObject { ["label"]="Group" };
        place.Siblings.RemoveAt(place.Index);
        place.Node["x"]=0; place.Node["y"]=0;
        group["children"]=new JsonArray(place.Node);
        place.Siblings.Insert(place.Index,group);
        return (Commit(scratch,root),groupId);
    }
    /// <summary>Replaces the group with its children (same order) at the same canvas positions. Literal
    /// opacities take on the group's opacity (a variable opacity moves onto children that have none); a hidden
    /// group hides its children. Refused when a variable-driven group opacity or visibility cannot be carried.</summary>
    public static NativeGraphicsScratch Ungroup(NativeGraphicsScratch scratch,string groupId)
    {
        var root=Read(scratch); var places=Places(root); var place=Find(places,groupId);
        if(TypeOf(place.Node)!="group") throw new InvalidDataException("Choose a group to ungroup.");
        if(place.Locked) throw new InvalidOperationException("Unlock the group before ungrouping it.");
        var group=place.Node; var list=group["children"] as JsonArray ?? new JsonArray(); var children=list.OfType<JsonObject>().ToArray();
        var flow=FlowOf(group); var padding=Literal(group,"padding"); var gap=Literal(group,"gap");
        var originX=Literal(group,"x")+padding; var originY=Literal(group,"y")+padding;
        double? opacity=group["opacity"] is null ? 1d : IsNumber(group,"opacity") ? Literal(group,"opacity") : (double?)null; // null = variable-bound
        var visibility=group["visible"]; double step=0;
        if(opacity is null && children.Any(c=>c["opacity"] is not null)) {
            var field=group["opacity"] is JsonObject o && o["field"] is JsonValue f && f.TryGetValue<string>(out var fieldId) ? fieldId : "";
            throw new InvalidOperationException($"This group's opacity follows the variable {field}. Clear it before ungrouping.");
        }
        if(visibility is JsonObject && children.Any(c=>c["visible"] is JsonObject))
            throw new InvalidOperationException("Two variables would drive this layer's visibility. Clear one before ungrouping.");
        list.Clear(); place.Siblings.RemoveAt(place.Index);
        for(var i=0;i<children.Length;i++) {
            var child=children[i];
            var x=originX+Literal(child,"x")+(flow=="horizontal" ? step : 0); var y=originY+Literal(child,"y")+(flow=="vertical" ? step : 0);
            if(flow is "vertical" or "horizontal") step+=Literal(child,flow=="vertical" ? "height" : "width")+gap;
            if(child["x"] is null || IsNumber(child,"x")) child["x"]=Number(x);
            if(child["y"] is null || IsNumber(child,"y")) child["y"]=Number(y);
            if(opacity is null) child["opacity"]=group["opacity"]!.DeepClone(); // no child opacity (checked above): carry the binding
            else if(opacity is double factor && factor!=1) {
                if(child["opacity"] is null) child["opacity"]=Number(factor);
                else if(IsNumber(child,"opacity")) child["opacity"]=Number(Literal(child,"opacity")*factor);
            }
            if(visibility is JsonValue literal && literal.TryGetValue<bool>(out var shown) && !shown) child["visible"]=false;
            else if(visibility is JsonObject binding && (child["visible"] is null || (child["visible"] is JsonValue own && own.TryGetValue<bool>(out var on) && on))) child["visible"]=binding.DeepClone();
            place.Siblings.Insert(place.Index+i,child);
        }
        return Commit(scratch,root);
    }
    /// <summary>Adds a text, shape or clock layer frontmost at the design root, with a readable id
    /// (<c>text-1</c>, <c>shape-1</c>, <c>clock-1</c>) and brand-token styling.</summary>
    public static (NativeGraphicsScratch Scratch,string Id) AddLayer(NativeGraphicsScratch scratch,string kind)
    {
        if(kind is not ("text" or "shape" or "clock")) throw new InvalidDataException("Choose a layer kind: text, shape or clock.");
        var root=Read(scratch); var places=Places(root);
        if(kind=="clock") {
            if(root["family"] is not JsonValue family || !family.TryGetValue<string>(out var name) || name is not ("clock" or "countdown"))
                throw new InvalidOperationException("Clocks tick in clock and countdown designs. Start one from the Clock look.");
            if(FindField(root,"time") is JsonObject time) {
                if(time["type"]?.GetValue<string>()!="string") throw new InvalidOperationException("The variable time must be text to show a clock.");
            } else {
                var fields=Fields(root); if(fields.Count>=MaxFields) throw new InvalidOperationException("A template supports at most 32 variables.");
                fields.Add(new JsonObject { ["id"]="time",["type"]=FieldKindString,["default"]="12:00:00",["bindable"]=true,["maxLength"]=32 });
            }
        }
        var id=NextId(places.Keys,kind);
        var node=kind=="shape"
            ? new JsonObject { ["id"]=id,[LayerTypeKey]="rect",["x"]=0,["y"]=0,["width"]=400,["height"]=120,
                ["fill"]=new JsonObject { ["token"]="brand.accent" },["cornerRadius"]=0,["metadata"]=new JsonObject { ["label"]="Shape" } }
            : new JsonObject { ["id"]=id,[LayerTypeKey]="text",["x"]=40,["y"]=40,["width"]=kind=="clock" ? 320 : 600,["height"]=72,
                ["text"]=kind=="clock" ? new JsonObject { ["field"]="time" } : "Text",["font"]=new JsonObject { ["token"]="brand.fontBody" },
                ["fontSize"]=42,["fontWeight"]=600,["color"]=new JsonObject { ["token"]="brand.onPrimary" },
                ["metadata"]=new JsonObject { ["label"]=kind=="clock" ? "Clock" : "Text" } };
        if(kind=="text" && SpotBelowText(root,72) is { } spot) { node["x"]=spot.X; node["y"]=spot.Y; node["width"]=spot.Width; }
        root["layers"]!.AsArray().Add(node); AddBrandTokens(root,node);
        return (Commit(scratch,root),id);
    }
    /// <summary>Where a new text line goes: 8 px under the lowest top-level text layer, at its x and width, when
    /// that still fits inside the template bounds (else null: the caller keeps 40,40).</summary>
    private static (double X,double Y,double Width)? SpotBelowText(JsonObject root,double height)
    {
        static double Num(JsonNode? value)=>value is JsonValue v && v.TryGetValue<double>(out var d) && double.IsFinite(d) ? d : 0;
        if(root["bounds"] is not JsonObject bounds || root["layers"] is not JsonArray layers) return null;
        var lowest=layers.OfType<JsonObject>().Where(l=>TypeOf(l)=="text")
            .OrderByDescending(l=>Num(l["y"])+Num(l["height"])).FirstOrDefault();
        if(lowest is null) return null;
        var (x,width,y)=(Num(lowest["x"]),Num(lowest["width"]),Num(lowest["y"])+Num(lowest["height"])+8);
        return width>=1 && x>=0 && x+width<=Num(bounds["width"]) && y+height<=Num(bounds["height"]) ? (x,y,width) : null;
    }
    /// <summary>Adds an image layer frontmost at the root. <paramref name="source"/> is <c>field:&lt;image variable&gt;</c>,
    /// <c>asset:&lt;declared asset&gt;</c> or <c>new-variable</c> (adds an optional image variable <c>image</c>, <c>image-2</c>, …).</summary>
    public static (NativeGraphicsScratch Scratch,string Id) AddImage(NativeGraphicsScratch scratch,string source)
    {
        var root=Read(scratch); var places=Places(root); JsonNode asset; var bound=true; source??="";
        if(source=="new-variable") {
            var fields=Fields(root); if(fields.Count>=MaxFields) throw new InvalidOperationException("A template supports at most 32 variables.");
            var n=1; string fieldId; while(FindField(root,fieldId=n==1 ? "image" : $"image-{n}") is not null) n++;
            fields.Add(new JsonObject { ["id"]=fieldId,["type"]=FieldKindImage,["default"]="",["bindable"]=true });
            asset=new JsonObject { ["field"]=fieldId };
        } else if(source.StartsWith("field:",StringComparison.Ordinal)) {
            var fieldId=source["field:".Length..];
            if(FindField(root,fieldId)?["type"]?.GetValue<string>()!="image") throw new InvalidDataException("Choose an image variable this design declares.");
            asset=new JsonObject { ["field"]=fieldId };
        } else if(source.StartsWith("asset:",StringComparison.Ordinal)) {
            var assetId=source["asset:".Length..];
            if(root["assets"] is not JsonArray declared || !declared.OfType<JsonObject>().Any(a=>a["id"] is JsonValue v && v.TryGetValue<string>(out var known) && known==assetId))
                throw new InvalidDataException("Choose an image asset this design declares.");
            asset=assetId; bound=false;
        } else throw new InvalidDataException("Choose an image variable, a declared image asset or a new image variable.");
        var id=NextId(places.Keys,"image");
        var node=new JsonObject { ["id"]=id,[LayerTypeKey]="image",["x"]=0,["y"]=0,["width"]=160,["height"]=160,["asset"]=asset,
            ["fit"]="contain",["align"]="center",["verticalAlign"]="center",["metadata"]=new JsonObject { ["label"]="Image" } };
        if(bound) node["collapseWhenEmpty"]=true;
        root["layers"]!.AsArray().Add(node);
        return (Commit(scratch,root),id);
    }
}
