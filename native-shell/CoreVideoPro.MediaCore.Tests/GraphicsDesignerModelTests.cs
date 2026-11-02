using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsDesignerModelTests
{
    private static NativeGraphicsScratch Seed=>NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest("lower-third","design-test-a1b2c3",1,"Test")));
    private static JsonElement Fields(string name,string title)=>JsonSerializer.SerializeToElement(new Dictionary<string,string> { ["name"]=name,["title"]=title });
    private static JsonElement Layer(NativeGraphicsScratch s,string id)=>GraphicsTemplateEditor.Layer(s,id);

    [Fact]
    public void RowsListFrontToBackWithKindsAndDetails()
    {
        var rows=GraphicsDesignerModel.Rows(Seed);
        Assert.Equal(new[] { "title-label","name-label","accent","plate" },rows.Select(r=>r.Id));
        Assert.Equal(new[] { "{title}","{name}","shape","shape" },rows.Select(r=>r.Detail));
        Assert.Equal(new[] { "Text","Text","Shape","Shape" },rows.Select(r=>r.Kind));
    }
    [Fact]
    public void GroupRowsNestTheirChildren()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        var rows=GraphicsDesignerModel.Rows(grouped);
        Assert.Equal(new[] { g,"title-label","name-label","accent","plate" },rows.Select(r=>r.Id));
        Assert.Equal((0,"group · 1 layers","Group"),(rows[0].Depth,rows[0].Detail,rows[0].Kind));
        Assert.Equal((1,g),(rows[1].Depth,rows[1].Parent));
    }
    [Fact]
    public void DropTargetMapsListPositionsToMoveTo()
    {
        var rows=GraphicsDesignerModel.Rows(Seed);
        Assert.Equal(("",0),GraphicsDesignerModel.DropTarget(rows,"plate",0,false));
        Assert.Equal(("",3),GraphicsDesignerModel.DropTarget(rows,"title-label",4,false));
        Assert.Equal(("",1),GraphicsDesignerModel.DropTarget(rows,"title-label",2,false));
    }
    [Fact]
    public void DropTargetRefusesCyclesAndLockedLayers()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        var rows=GraphicsDesignerModel.Rows(grouped);
        Assert.Null(GraphicsDesignerModel.DropTarget(rows,g,1,false));
        Assert.Equal((g,0),GraphicsDesignerModel.DropTarget(rows,"accent",0,true));
        var locked=GraphicsDesignerModel.Rows(GraphicsTemplateEditor.Lock(Seed,"plate",true));
        Assert.Null(GraphicsDesignerModel.DropTarget(locked,"plate",0,false));
        var lockedGroup=GraphicsDesignerModel.Rows(GraphicsTemplateEditor.Lock(grouped,g,true));
        Assert.Null(GraphicsDesignerModel.DropTarget(lockedGroup,"accent",0,true));
    }
    [Fact]
    public void InspectorResolvesBoundValuesAndTokens()
    {
        var i=GraphicsDesignerModel.Inspect(Seed,Fields("Darnell Reyes","Guest"),"name-label");
        Assert.Equal(("name","brand.fontBody",42d,600,"brand.onPrimary"),(i.ShowsVariable,i.FontChoice,i.FontSize,i.FontWeight,i.ColourChoice));
        Assert.Equal((32d,27d,996d,62d,1d),(i.X,i.Y,i.Width,i.Height,i.Opacity));
        Assert.Equal((1,"ellipsis","left"),(i.MaxLines,i.Overflow,i.Align));
    }
    [Fact]
    public void InspectorBoundNumberUsesTheDraftValueElseTheVariableDefault()
    {
        var scratch=GraphicsTemplateEditor.AddField(Seed,"size","number");
        scratch=GraphicsTemplateEditor.SetFieldOptions(scratch,"size",null,JsonSerializer.SerializeToElement(30),false);
        scratch=GraphicsTemplateEditor.BindLayer(scratch,"name-label","fontSize","size",null);
        var withValue=JsonSerializer.SerializeToElement(new Dictionary<string,object> { ["name"]="Ada",["title"]="Host",["size"]=55 });
        Assert.Equal(55d,GraphicsDesignerModel.Inspect(scratch,withValue,"name-label").FontSize);
        Assert.Equal(30d,GraphicsDesignerModel.Inspect(scratch,Fields("Ada","Host"),"name-label").FontSize);
    }
    [Fact]
    public void FontChoicesAppendTheLayersCustomFamily()
    {
        Assert.Equal(GraphicsDesignerChoices.Fonts,GraphicsDesignerModel.FontChoices("brand.fontBody"));
        Assert.Equal(GraphicsDesignerChoices.Fonts,GraphicsDesignerModel.FontChoices(""));
        var custom=GraphicsDesignerModel.FontChoices("Papyrus");
        Assert.Equal(GraphicsDesignerChoices.Fonts.Count+1,custom.Count);
        Assert.Equal(("Papyrus","Papyrus"),custom[^1]);
    }
    [Fact]
    public void MoveLayerSaysWhenTheGroupShiftedTheLayer()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        grouped=GraphicsTemplateEditor.Set(grouped,g,"padding",JsonSerializer.SerializeToElement(20));
        // accent sits at (0,0); the padded group's content box starts at (32+20, 94+20) and the group cannot move left of 0,
        // so the layer lands inside the padding, not where it was.
        var (moved,shifted)=GraphicsDesignerModel.MoveLayer(grouped,"accent",g,0,null);
        Assert.True(shifted);
        Assert.Equal(g,GraphicsTemplateEditor.Layers(moved).Single(l=>l.Id=="accent").Parent);
        var (same,kept)=GraphicsDesignerModel.MoveLayer(Seed,"plate","",0,null);
        Assert.False(kept);
        Assert.Equal("plate",GraphicsDesignerModel.Rows(same)[0].Id);
        // An unpadded free-layout group grows to take the layer where it stands: no shift, no message.
        var (free,freeGroup)=GraphicsTemplateEditor.Group(Seed,"name-label");
        Assert.False(GraphicsDesignerModel.MoveLayer(free,"title-label",freeGroup,0,null).Shifted);
        // The native node's position wins over the document's literal one when given.
        Assert.True(GraphicsDesignerModel.MoveLayer(free,"title-label",freeGroup,0,(-40,94)).Shifted);
    }
    [Fact]
    public void SetColourAcceptsBrandTokensAndHexOnly()
    {
        Assert.Equal("brand.accent",Layer(GraphicsDesignerModel.SetColour(Seed,"plate","fill","brand.accent"),"plate").GetProperty("fill").GetProperty("token").GetString());
        Assert.Equal("#FF8800",Layer(GraphicsDesignerModel.SetColour(Seed,"plate","fill","#FF8800"),"plate").GetProperty("fill").GetString());
        Assert.Equal("Use a brand colour or a colour like #RRGGBB.",Assert.Throws<InvalidDataException>(()=>GraphicsDesignerModel.SetColour(Seed,"plate","fill","orange")).Message);
    }
    [Fact]
    public void SetTypefaceAcceptsBrandFontsAndListedFamilies()
    {
        Assert.Equal("Bahnschrift",Layer(GraphicsDesignerModel.SetTypeface(Seed,"name-label","Bahnschrift"),"name-label").GetProperty("font").GetString());
        Assert.Equal("brand.fontHeading",Layer(GraphicsDesignerModel.SetTypeface(Seed,"name-label","brand.fontHeading"),"name-label").GetProperty("font").GetProperty("token").GetString());
        Assert.Equal("Choose a brand font or one of the listed typefaces.",Assert.Throws<InvalidDataException>(()=>GraphicsDesignerModel.SetTypeface(Seed,"name-label","Comic Sans MS")).Message);
    }
    [Fact]
    public void SetShowsSwitchesBetweenVariableAndFixedText()
    {
        var fixedText=GraphicsDesignerModel.SetShows(Seed,"title-label",null,"Chicago");
        Assert.Equal("Chicago",Layer(fixedText,"title-label").GetProperty("text").GetString());
        Assert.Equal("title",Layer(GraphicsDesignerModel.SetShows(fixedText,"title-label","title",""),"title-label").GetProperty("text").GetProperty("field").GetString());
    }
    [Fact]
    public void SelectionStaysElseFollowsARestoredLayerElseTheFrontmost()
    {
        var rows=GraphicsDesignerModel.Rows(Seed);
        Assert.Equal("accent",GraphicsDesignerModel.SelectionAfter(["title-label","name-label","accent","plate"],rows,"accent"));
        // Undo of a Delete brings plate back: select it.
        Assert.Equal("plate",GraphicsDesignerModel.SelectionAfter(["title-label","name-label","accent"],rows,"gone"));
        // Undo of an Add removes the selected layer: the front-most row.
        Assert.Equal("title-label",GraphicsDesignerModel.SelectionAfter(["text-1","title-label","name-label","accent","plate"],rows,"text-1"));
        Assert.Null(GraphicsDesignerModel.SelectionAfter([],[],"x"));
    }
    [Fact]
    public void RowsSignatureIgnoresSelectionButNotStructure()
    {
        var rows=GraphicsDesignerModel.Rows(Seed);
        Assert.Equal(GraphicsDesignerModel.RowsSignature(rows),GraphicsDesignerModel.RowsSignature(GraphicsDesignerModel.Rows(Seed)));
        Assert.NotEqual(GraphicsDesignerModel.RowsSignature(rows),GraphicsDesignerModel.RowsSignature(GraphicsDesignerModel.Rows(GraphicsTemplateEditor.SetLabel(Seed,"plate","Backing"))));
        Assert.NotEqual(GraphicsDesignerModel.RowsSignature(rows),GraphicsDesignerModel.RowsSignature(GraphicsDesignerModel.Rows(GraphicsTemplateEditor.Lock(Seed,"plate",true))));
    }
    [Fact]
    public void ImageSourcesListImageVariablesThenDeclaredAssets()
    {
        var manifest=GraphicsDesignSeeds.Manifest("bug","design-test-a1b2c3",1,"Test");
        var scratch=NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(manifest));
        var sources=GraphicsDesignerModel.ImageSources(scratch);
        var images=GraphicsTemplateEditor.TemplateFields(scratch).Where(f=>f.Type=="image").Select(f=>"field:"+f.Id);
        Assert.Equal(images,sources.Where(s=>s.Source.StartsWith("field:")).Select(s=>s.Source));
        Assert.All(sources,s=>Assert.False(string.IsNullOrWhiteSpace(s.Label)));
        manifest["assets"]=new JsonArray(new JsonObject { ["id"]="logo",["file"]="assets/a.png",["sha256"]=new string('a',64) });
        var withAsset=GraphicsDesignerModel.ImageSources(NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(manifest)));
        Assert.Equal(("asset:logo","Image: logo"),withAsset[^1]);
    }
    [Fact]
    public void PendingTextCommitsOnceForItsOwnLayerBeforeTheSelectionMoves()
    {
        var pending=new GraphicsPendingEdit(); var committed=new List<string>();
        pending.Track("text-1",()=>{ committed.Add("text-1"); return true; });
        Assert.Equal("text-1",pending.Owner);
        Assert.True(pending.Flush());
        Assert.False(pending.Flush()); // once
        Assert.Equal(new[] { "text-1" },committed);
        // A commit that triggers a refresh (which flushes again) does not run twice.
        pending.Track("plate",()=>{ committed.Add("plate"); pending.Flush(); return true; });
        pending.Flush();
        Assert.Equal(new[] { "text-1","plate" },committed);
        pending.Track("a",()=>{ committed.Add("a"); return true; }); pending.Clear(); Assert.False(pending.Flush());
        Assert.Equal("",pending.Owner);
    }
}
