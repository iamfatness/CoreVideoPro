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

    private static double Number(JsonElement layer,string property,JsonElement draft,double fallback)
    {
        if(layer.ValueKind!=JsonValueKind.Object || !layer.TryGetProperty(property,out var v)) return fallback;
        if(v.ValueKind==JsonValueKind.Number) return v.GetDouble();
        if(FieldOf(layer,property) is { } field && draft.ValueKind==JsonValueKind.Object && draft.TryGetProperty(field,out var d) && d.ValueKind==JsonValueKind.Number) return d.GetDouble();
        return fallback;
    }

    public static GraphicsLayerInspection Inspect(NativeGraphicsScratch s,JsonElement draftFields,string id)
    {
        var row=Rows(s).FirstOrDefault(r=>r.Id==id) ?? throw new InvalidDataException("Layer was removed.");
        var n=GraphicsTemplateEditor.Layer(s,id);
        var gradient=n.TryGetProperty("fill",out var fill) && fill.ValueKind==JsonValueKind.Object && fill.TryGetProperty("stops",out _);
        var image=FieldOf(n,"asset") is { } af ? "field:"+af : TokenOf(n,"asset") is { } at ? "token:"+at : Str(n,"asset") is { } a ? "asset:"+a : "";
        return new GraphicsLayerInspection(id,row.Kind,row.Label,row.Locked,FieldOf(n,"text"),Str(n,"text") ?? "",Choice(n,"font"),Number(n,"fontSize",draftFields,0),
            (int)Number(n,"fontWeight",draftFields,400),Choice(n,"color"),(int)Number(n,"maxLines",draftFields,1),Str(n,"overflow") ?? "ellipsis",Str(n,"align") ?? "left",
            Number(n,"x",draftFields,0),Number(n,"y",draftFields,0),Number(n,"width",draftFields,0),Number(n,"height",draftFields,0),Number(n,"opacity",draftFields,1),
            gradient ? "" : Choice(n,"fill"),gradient,Number(n,"cornerRadius",draftFields,0),image,Str(n,"fit") ?? "contain",Str(n,"align") ?? "center",Str(n,"verticalAlign") ?? "center");
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
