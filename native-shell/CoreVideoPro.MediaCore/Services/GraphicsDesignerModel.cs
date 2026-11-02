using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>One line of the Designer's layer list. <see cref="Kind"/> is Text, Shape, Image, Clock (text bound to <c>time</c>) or Group.</summary>
public sealed record GraphicsDesignerRow(string Id,string Kind,string Label,string Detail,int Depth,string Parent,bool Locked,bool Visible,bool VisibilityBound);

/// <summary>What the Designer's inspector shows for one layer; field-bound numbers resolve to the draft value.
/// <see cref="ImageSource"/> is <c>field:&lt;id&gt;</c>, <c>asset:&lt;id&gt;</c> or <c>token:brand.logo</c>.</summary>
public sealed record GraphicsLayerInspection(string Id,string Kind,string Label,bool Locked,string? ShowsVariable,string FixedText,string FontChoice,double FontSize,int FontWeight,
    string ColourChoice,int MaxLines,string Overflow,string Align,double X,double Y,double Width,double Height,double Opacity,string FillChoice,bool FillGradient,double CornerRadius,
    string ImageSource,string Fit,string ImageAlign,string ImageVerticalAlign);

public static class GraphicsDesignerChoices
{
    public static IReadOnlyList<(string Id,string Label)> Colours { get; } = [("brand.primary","Brand · plate"),("brand.accent","Brand · accent"),("brand.background","Brand · background"),
        ("brand.foreground","Brand · foreground"),("brand.secondary","Brand · secondary"),("brand.onPrimary","Brand · ink"),("brand.onPrimaryMuted","Brand · ink muted")];
    public static IReadOnlyList<(string Id,string Label)> Fonts { get; } =
        [("brand.fontHeading","Brand heading"),("brand.fontBody","Brand body"),.. GraphicsBuilder.TypefaceChoices.Select(f=>(f,f))];
    public static IReadOnlyList<(int Id,string Label)> Weights { get; } = [(400,"Regular"),(500,"Medium"),(600,"Semibold"),(700,"Bold")];
    public static IReadOnlyList<int> Lines { get; } = [1,2,3];
    public static IReadOnlyList<(string Id,string Label)> Overflows { get; } = [("ellipsis","Ellipsis"),("wrap-then-ellipsis","Wrap, then ellipsis")];
    public static IReadOnlyList<(string Id,string Label)> Fits { get; } = [("contain","Contain"),("cover","Cover")];
    public static IReadOnlyList<(string Id,string Label)> Aligns { get; } = [("left","Left"),("center","Centre"),("right","Right")];
    public static IReadOnlyList<(string Id,string Label)> VerticalAligns { get; } = [("top","Top"),("center","Middle"),("bottom","Bottom")];
}

/// <summary>Non-UI model behind the Designer screen: layer rows, drag-and-drop targets and the inspector.</summary>
public static class GraphicsDesignerModel
{
    private static readonly System.Text.RegularExpressions.Regex Hex=new(@"\A#([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})\z",System.Text.RegularExpressions.RegexOptions.CultureInvariant);

    private static string? Str(JsonElement layer,string property)=>layer.ValueKind==JsonValueKind.Object && layer.TryGetProperty(property,out var v) && v.ValueKind==JsonValueKind.String ? v.GetString() : null;
    private static string? Ref(JsonElement layer,string property,string key)=>layer.ValueKind==JsonValueKind.Object && layer.TryGetProperty(property,out var v) && v.ValueKind==JsonValueKind.Object
        && v.TryGetProperty(key,out var r) && r.ValueKind==JsonValueKind.String ? r.GetString() : null;
    private static string? FieldOf(JsonElement layer,string property)=>Ref(layer,property,"field");
    private static string? TokenOf(JsonElement layer,string property)=>Ref(layer,property,"token");
    private static string Choice(JsonElement layer,string property)=>TokenOf(layer,property) ?? Str(layer,property) ?? "";

    /// <summary>Layers front-to-back, depth-first: a group, then its children front-to-back.</summary>
    public static IReadOnlyList<GraphicsDesignerRow> Rows(NativeGraphicsScratch s)
    {
        var layers=GraphicsTemplateEditor.Layers(s); var result=new List<GraphicsDesignerRow>();
        void Emit(string parent)
        {
            foreach(var l in layers.Where(x=>x.Parent==parent).Reverse()) {
                var node=GraphicsTemplateEditor.Layer(s,l.Id);
                var shows=FieldOf(node,"text"); var asset=FieldOf(node,"asset");
                var (kind,detail)=l.Type switch {
                    "group"=>("Group",$"group · {layers.Count(x=>x.Parent==l.Id)} layers"),
                    "rect"=>("Shape","shape"),
                    "image"=>("Image",asset is null ? "image" : "{"+asset+"}"),
                    _=>shows is null ? ("Text",Cut(Str(node,"text") ?? "")) : (shows=="time" ? "Clock" : "Text","{"+shows+"}") };
                var bound=node.ValueKind==JsonValueKind.Object && node.TryGetProperty("visible",out var v) && v.ValueKind==JsonValueKind.Object;
                result.Add(new GraphicsDesignerRow(l.Id,kind,l.Label,detail,l.Depth,l.Parent,l.Locked,l.Visible,bound));
                Emit(l.Id);
            }
        }
        Emit(""); return result;
    }
    private static string Cut(string text)=>text.Length<=24 ? text : text[..(char.IsHighSurrogate(text[23]) ? 23 : 24)];

    /// <summary>Maps a drop position to <c>MoveTo</c> arguments; null when the drop is not allowed.</summary>
    public static (string Parent,int FrontIndex)? DropTarget(IReadOnlyList<GraphicsDesignerRow> rows,string draggedId,int insertionRow,bool ontoGroup)
    {
        var draggedAt=-1; for(var i=0;i<rows.Count;i++) if(rows[i].Id==draggedId) draggedAt=i;
        if(draggedAt<0 || rows[draggedAt].Locked || insertionRow<0 || insertionRow>rows.Count) return null;
        var dragged=rows[draggedAt]; string parent; int front;
        if(insertionRow<rows.Count && ontoGroup && rows[insertionRow].Kind=="Group") { parent=rows[insertionRow].Id; front=0; }
        else {
            parent=insertionRow==rows.Count ? "" : rows[insertionRow].Parent;
            front=rows.Take(insertionRow).Count(r=>r.Parent==parent);
            if(dragged.Parent==parent && draggedAt<insertionRow) front--; // the layer leaves its old slot first
        }
        if(parent!="") {
            for(var cursor=rows.FirstOrDefault(r=>r.Id==parent);cursor is not null;cursor=cursor.Parent=="" ? null : rows.FirstOrDefault(r=>r.Id==cursor.Parent))
                if(cursor.Id==draggedId) return null;
            if(rows.First(r=>r.Id==parent).Locked) return null;
        }
        return (parent,Math.Max(0,front));
    }

    // A literal number; a variable-bound one resolves to the draft's value, else the variable's own default (sample).
    private static double Number(JsonElement layer,string property,JsonElement draft,IReadOnlyList<NativeGraphicsTemplateField> fields,double fallback)
    {
        if(layer.ValueKind!=JsonValueKind.Object || !layer.TryGetProperty(property,out var v)) return fallback;
        if(v.ValueKind==JsonValueKind.Number) return v.GetDouble();
        if(FieldOf(layer,property) is not { } field) return fallback;
        if(draft.ValueKind==JsonValueKind.Object && draft.TryGetProperty(field,out var d) && d.ValueKind==JsonValueKind.Number) return d.GetDouble();
        return fields.FirstOrDefault(f=>f.Id==field)?.Default is { ValueKind: JsonValueKind.Number } sample ? sample.GetDouble() : fallback;
    }

    public static GraphicsLayerInspection Inspect(NativeGraphicsScratch s,JsonElement draftFields,string id)
    {
        var row=Rows(s).FirstOrDefault(r=>r.Id==id) ?? throw new InvalidDataException("Layer was removed.");
        var n=GraphicsTemplateEditor.Layer(s,id);
        var gradient=n.TryGetProperty("fill",out var fill) && fill.ValueKind==JsonValueKind.Object && fill.TryGetProperty("stops",out _);
        var fields=GraphicsTemplateEditor.TemplateFields(s);
        double Number(string property,double fallback)=>GraphicsDesignerModel.Number(n,property,draftFields,fields,fallback);
        var image=FieldOf(n,"asset") is { } af ? "field:"+af : TokenOf(n,"asset") is { } at ? "token:"+at : Str(n,"asset") is { } a ? "asset:"+a : "";
        return new GraphicsLayerInspection(id,row.Kind,row.Label,row.Locked,FieldOf(n,"text"),Str(n,"text") ?? "",Choice(n,"font"),Number("fontSize",0),
            (int)Number("fontWeight",400),Choice(n,"color"),(int)Number("maxLines",1),Str(n,"overflow") ?? "ellipsis",Str(n,"align") ?? "left",
            Number("x",0),Number("y",0),Number("width",0),Number("height",0),Number("opacity",1),
            gradient ? "" : Choice(n,"fill"),gradient,Number("cornerRadius",0),image,Str(n,"fit") ?? "contain",Str(n,"align") ?? "center",Str(n,"verticalAlign") ?? "center");
    }

    /// <summary>What the layers list must rebuild for: the rows' structure and state, never the selection
    /// (selection restyles rows in place, so a double-click's second tap lands on the same row).</summary>
    public static string RowsSignature(IReadOnlyList<GraphicsDesignerRow> rows)=>
        string.Join('|',rows.Select(r=>$"{r.Id}:{r.Kind}:{r.Label}:{r.Detail}:{r.Depth}:{r.Parent}:{r.Locked}:{r.Visible}:{r.VisibilityBound}"));

    /// <summary>The layer to select after the rows changed: the current one while it exists; else a layer that has just
    /// (re)appeared, e.g. one an Undo of Delete restored; else the front-most. Null when there are no rows.</summary>
    public static string? SelectionAfter(IReadOnlyCollection<string> previousIds,IReadOnlyList<GraphicsDesignerRow> rows,string selected)
    {
        if(rows.Count==0) return null;
        if(rows.Any(r=>r.Id==selected)) return selected;
        var known=previousIds as ISet<string> ?? previousIds.ToHashSet(StringComparer.Ordinal);
        return previousIds.Count>0 && rows.FirstOrDefault(r=>!known.Contains(r.Id)) is { } restored ? restored.Id : rows[0].Id;
    }

    /// <summary>Image sources a layer can use: the design's image variables (<c>field:id</c>), then its declared image
    /// assets (<c>asset:id</c>).</summary>
    public static IReadOnlyList<(string Source,string Label)> ImageSources(NativeGraphicsScratch s)
    {
        var sources=GraphicsTemplateEditor.TemplateFields(s).Where(f=>f.Type=="image").Select(f=>("field:"+f.Id,"Variable: "+f.Id)).ToList();
        using var manifest=JsonDocument.Parse(s.Manifest);
        if(manifest.RootElement.TryGetProperty("assets",out var assets) && assets.ValueKind==JsonValueKind.Array)
            foreach(var asset in assets.EnumerateArray())
                if(asset.ValueKind==JsonValueKind.Object && asset.TryGetProperty("id",out var id) && id.ValueKind==JsonValueKind.String && id.GetString() is { Length: > 0 } assetId)
                    sources.Add(("asset:"+assetId,"Image: "+assetId));
        return sources;
    }

    /// <summary>The Typeface list for a layer: the brand fonts and listed families, plus the layer's own custom family
    /// (appended) when it uses one, so the list always contains the current choice.</summary>
    public static IReadOnlyList<(string Id,string Label)> FontChoices(string current)=>
        string.IsNullOrEmpty(current) || GraphicsDesignerChoices.Fonts.Any(f=>f.Id==current) ? GraphicsDesignerChoices.Fonts : [.. GraphicsDesignerChoices.Fonts,(current,current)];

    /// <summary><see cref="GraphicsTemplateEditor.MoveTo"/> plus whether the layer ended up somewhere else on the canvas than it was
    /// (or than <paramref name="absolute"/>, the native node position): a padded or nested group could not take it where it stood.</summary>
    public static (NativeGraphicsScratch Scratch,bool Shifted) MoveLayer(NativeGraphicsScratch s,string id,string parent,int frontIndex,(double X,double Y)? absolute)
    {
        var before=absolute ?? GraphicsTemplateEditor.CanvasPositionOf(s,id);
        var parentBefore=GraphicsTemplateEditor.Layers(s).FirstOrDefault(l=>l.Id==id)?.Parent;
        var moved=GraphicsTemplateEditor.MoveTo(s,id,parent,frontIndex,absolute);
        if(parentBefore==(parent ?? "")) return (moved,false);
        var after=GraphicsTemplateEditor.CanvasPositionOf(moved,id);
        return (moved,Math.Abs(after.X-before.X)>0.5 || Math.Abs(after.Y-before.Y)>0.5);
    }

    private static JsonElement Token(string token)=>JsonSerializer.SerializeToElement(new Dictionary<string,string> { ["token"]=token });

    public static NativeGraphicsScratch SetColour(NativeGraphicsScratch s,string id,string property,string choice)
    {
        if(property is not ("color" or "fill")) throw new InvalidDataException("Choose the text colour or the fill.");
        choice??="";
        JsonElement value;
        if(GraphicsDesignerChoices.Colours.Any(c=>c.Id==choice)) value=Token(choice);
        else if(Hex.IsMatch(choice)) value=JsonSerializer.SerializeToElement(choice);
        else throw new InvalidDataException("Use a brand colour or a colour like #RRGGBB.");
        return GraphicsTemplateEditor.BindLayer(s,id,property,null,value);
    }
    public static NativeGraphicsScratch SetTypeface(NativeGraphicsScratch s,string id,string choice)
    {
        choice??="";
        JsonElement value;
        if(choice is "brand.fontHeading" or "brand.fontBody") value=Token(choice);
        else if(GraphicsBuilder.TypefaceChoices.Contains(choice) || (choice.Length!=0 && Str(GraphicsTemplateEditor.Layer(s,id),"font")==choice)) value=JsonSerializer.SerializeToElement(choice);
        else throw new InvalidDataException("Choose a brand font or one of the listed typefaces.");
        return GraphicsTemplateEditor.BindLayer(s,id,"font",null,value);
    }
    public static NativeGraphicsScratch SetShows(NativeGraphicsScratch s,string id,string? variable,string fixedText)=>
        variable is null ? GraphicsTemplateEditor.BindLayer(s,id,"text",null,JsonSerializer.SerializeToElement(fixedText ?? ""))
            : GraphicsTemplateEditor.BindLayer(s,id,"text",variable,null);
}
