using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Layer structure edits the Designer's layers panel calls: rename, hide, reorder/move,
/// group/ungroup, add and duplicate. Every result must stay a document native accepts.</summary>
public sealed class GraphicsTemplateEditorStructureTests
{
    private static JsonObject SeedObject()=>GraphicsDesignSeeds.Manifest("lower-third","design-test-a1b2c3",1,"Test");
    private static NativeGraphicsScratch From(JsonObject manifest)=>NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(manifest));
    private static NativeGraphicsScratch Seed=>From(SeedObject());
    private static NativeGraphicsScratch Bound
    {
        get {
            var manifest=SeedObject();
            manifest["fields"]!.AsArray().Add(new JsonObject { ["id"]="show-title",["type"]="boolean",["default"]=true,["bindable"]=true });
            manifest["layers"]!.AsArray().OfType<JsonObject>().Single(l=>l["id"]!.GetValue<string>()=="title-label")["visible"]=new JsonObject { ["field"]="show-title" };
            return From(manifest);
        }
    }
    // The corporate-lower-third manifest (native/src/graphics/GraphicsTemplate.cpp corporateLowerThird()).
    private static NativeGraphicsScratch Corporate=>NativeGraphicsScratch.From(JsonDocument.Parse("""
    {"schemaVersion":1,"templateId":"corporate-lower-third","templateVersion":1,"name":"Corporate lower third","family":"lower-third",
      "canvas":{"width":1920,"height":1080},"bounds":{"width":1050,"height":176},
      "brandTokens":["brand.primary","brand.onPrimary","brand.onPrimaryMuted","brand.fontBody"],
      "fields":[{"id":"name","type":"string","default":"","required":true,"bindable":true,"maxLength":256},
        {"id":"title","type":"string","default":"","required":false,"bindable":true,"maxLength":512}],
      "layers":[{"id":"plate","type":"rect","x":0,"y":0,"width":1050,"height":176,"fill":{"token":"brand.primary"},"opacity":0.92,"cornerRadius":8},
        {"id":"text-stack","type":"group","x":24,"y":20,"width":1002,"height":136,"flow":"vertical","gap":8,"children":[
          {"id":"name-text","type":"text","text":{"field":"name"},"width":1002,"height":62,"font":{"token":"brand.fontBody"},"fontWeight":600,"fontSize":42,"lineHeight":1.2,"color":{"token":"brand.onPrimary"},"align":"left","maxLines":1,"overflow":"ellipsis"},
          {"id":"title-text","type":"text","text":{"field":"title"},"width":1002,"height":42,"font":{"token":"brand.fontBody"},"fontWeight":400,"fontSize":28,"lineHeight":1.2,"color":{"token":"brand.onPrimaryMuted"},"align":"left","maxLines":1,"overflow":"ellipsis","collapseWhenEmpty":true}]}],
      "animations":{"in":{"type":"slide","direction":"left","durationMs":400,"easing":"ease-out"},"out":{"type":"fade","durationMs":300,"easing":"linear"}}}
    """).RootElement.Clone());

    private static double Num(JsonElement layer,string property)=>layer.TryGetProperty(property,out var value) ? value.GetDouble() : 0;
    private static (double,double) Pos(NativeGraphicsScratch s,string id){ var l=GraphicsTemplateEditor.Layer(s,id); return (Num(l,"x"),Num(l,"y")); }
    private static (double,double,double,double) Box(NativeGraphicsScratch s,string id){ var l=GraphicsTemplateEditor.Layer(s,id); return (Num(l,"x"),Num(l,"y"),Num(l,"width"),Num(l,"height")); }
    private static IEnumerable<string> ChildIds(NativeGraphicsScratch s,string id)=>GraphicsTemplateEditor.Layer(s,id).GetProperty("children").EnumerateArray().Select(c=>c.GetProperty("id").GetString()!);
    private static JsonElement Root(NativeGraphicsScratch s)=>JsonDocument.Parse(s.Manifest).RootElement;

    [Fact]
    public void GroupThenUngroupKeepsCanvasPositions()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        Assert.Equal("group-1",g);
        Assert.Equal((32d,94d,996d,42d),Box(grouped,g)); Assert.Equal((0d,0d),Pos(grouped,"title-label"));
        Assert.Equal("Group",GraphicsTemplateEditor.Layers(grouped).Single(l=>l.Id==g).Label);
        Assert.Equal("none",GraphicsTemplateEditor.Layer(grouped,g).GetProperty("flow").GetString());
        var back=GraphicsTemplateEditor.Ungroup(grouped,g);
        Assert.Equal((32d,94d),Pos(back,"title-label"));
        Assert.Equal(new[] { "plate","accent","name-label","title-label" },GraphicsTemplateEditor.Layers(back).Select(l=>l.Id));
    }
    [Fact]
    public void MoveIntoAGroupKeepsCanvasPositionAndGrowsTheGroup()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        var moved=GraphicsTemplateEditor.MoveTo(grouped,"name-label",g,0);
        Assert.Equal((32d,27d,996d,109d),Box(moved,g));
        Assert.Equal((0d,0d),Pos(moved,"name-label")); Assert.Equal((0d,67d),Pos(moved,"title-label"));
        Assert.Equal("name-label",ChildIds(moved,g).Last()); // frontIndex 0 = last in document order
        var outAgain=GraphicsTemplateEditor.MoveTo(moved,"name-label","",0);
        Assert.Equal((32d,27d),Pos(outAgain,"name-label")); Assert.Equal("name-label",GraphicsTemplateEditor.Layers(outAgain).Last().Id);
        var placed=GraphicsTemplateEditor.MoveTo(moved,"name-label","",3,(100d,50d));
        Assert.Equal((100d,50d),Pos(placed,"name-label")); Assert.Equal("name-label",GraphicsTemplateEditor.Layers(placed)[0].Id);
    }
    [Fact]
    public void MovingAGroupIntoItsOwnChildIsRefused()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        var (twice,outer)=GraphicsTemplateEditor.Group(grouped,g);
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.MoveTo(twice,outer,g,0));
        Assert.Equal("A group cannot be moved inside itself.",ex.Message);
    }
    [Fact]
    public void UngroupingAFlowGroupUsesFlowPositions()
    {
        var flat=GraphicsTemplateEditor.Ungroup(Corporate,"text-stack");
        Assert.Equal((24d,20d),Pos(flat,"name-text")); Assert.Equal((24d,90d),Pos(flat,"title-text"));
        Assert.Equal(new[] { "plate","name-text","title-text" },GraphicsTemplateEditor.Layers(flat).Select(l=>l.Id));
    }
    [Fact]
    public void UngroupMultipliesOpacity()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"accent");
        grouped=GraphicsTemplateEditor.Set(grouped,g,"opacity",JsonSerializer.SerializeToElement(0.5));
        grouped=GraphicsTemplateEditor.Set(grouped,"accent","opacity",JsonSerializer.SerializeToElement(0.8));
        var flat=GraphicsTemplateEditor.Ungroup(grouped,g);
        Assert.Equal(0.4,GraphicsTemplateEditor.Layer(flat,"accent").GetProperty("opacity").GetDouble(),9);
    }
    [Fact]
    public void UngroupingAHiddenGroupHidesItsChildren()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"accent");
        var flat=GraphicsTemplateEditor.Ungroup(GraphicsTemplateEditor.SetVisible(grouped,g,false),g);
        Assert.False(GraphicsTemplateEditor.Layers(flat).Single(l=>l.Id=="accent").Visible);
    }
    [Fact]
    public void HideAndShowWriteNativeVisibility()
    {
        var hidden=GraphicsTemplateEditor.SetVisible(Seed,"name-label",false);
        Assert.False(GraphicsTemplateEditor.Layer(hidden,"name-label").GetProperty("visible").GetBoolean());
        Assert.False(GraphicsTemplateEditor.Layers(hidden).Single(l=>l.Id=="name-label").Visible);
        Assert.False(GraphicsTemplateEditor.Layer(GraphicsTemplateEditor.SetVisible(hidden,"name-label",true),"name-label").TryGetProperty("visible",out _));
        var locked=GraphicsTemplateEditor.Lock(Seed,"plate",true);
        Assert.False(GraphicsTemplateEditor.Layers(GraphicsTemplateEditor.SetVisible(locked,"plate",false)).Single(l=>l.Id=="plate").Visible);
    }
    [Fact]
    public void HidingAVariableBoundLayerIsRefused()
    {
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.SetVisible(Bound,"title-label",false));
        Assert.Equal("This layer's visibility follows the variable show-title.",ex.Message);
    }
    [Fact]
    public void RenameWritesMetadataLabelEvenWhenLocked()
    {
        var locked=GraphicsTemplateEditor.Lock(Seed,"plate",true);
        var renamed=GraphicsTemplateEditor.SetLabel(locked,"plate","  Plate shape ");
        Assert.Equal("Plate shape",GraphicsTemplateEditor.Layers(renamed).Single(l=>l.Id=="plate").Label);
        Assert.True(GraphicsTemplateEditor.Layer(renamed,"plate").GetProperty("metadata").GetProperty("editorLocked").GetBoolean());
        var ex=Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetLabel(Seed,"plate",new string('a',65)));
        Assert.Equal("Use a layer name of 1–64 characters.",ex.Message);
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetLabel(Seed,"plate","  "));
    }
    [Fact]
    public void DeletingAGroupThatHoldsTheRequiredNameIsRefused()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"name-label");
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Remove(grouped,g));
        Assert.Equal("Required variable name must stay on a layer.",ex.Message);
    }
    [Fact]
    public void GroupingRespectsDepthAndLayerLimits()
    {
        var nested=Seed;
        for(var i=0;i<8;i++) nested=GraphicsTemplateEditor.Group(nested,"name-label").Scratch;
        Assert.Equal(8,GraphicsTemplateEditor.Layers(nested).Single(l=>l.Id=="name-label").Depth);
        var deep=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Group(nested,"name-label"));
        Assert.Equal("Template group nesting exceeds eight levels.",deep.Message);

        var manifest=SeedObject(); var layers=manifest["layers"]!.AsArray();
        for(var i=0;i<60;i++) layers.Add(new JsonObject { ["id"]=$"fill-{i}",["type"]="rect",["x"]=0,["y"]=0,["width"]=10,["height"]=10,["fill"]="#000000" });
        var full=From(manifest); Assert.Equal(64,GraphicsTemplateEditor.Layers(full).Count);
        var many=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Group(full,"plate"));
        Assert.Equal("A template supports at most 64 layers.",many.Message);
    }
    [Fact]
    public void LockedLayersCannotBeMovedGroupedOrDeleted()
    {
        var locked=GraphicsTemplateEditor.Lock(Seed,"plate",true);
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.MoveTo(locked,"plate","",0));
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Group(locked,"plate"));
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Remove(locked,"plate"));
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"accent"); var lockedGroup=GraphicsTemplateEditor.Lock(grouped,g,true);
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Ungroup(lockedGroup,g));
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.MoveTo(lockedGroup,"plate",g,0));
    }
    [Fact]
    public void ReorderWithinSiblings()
    {
        var moved=GraphicsTemplateEditor.MoveTo(Seed,"plate","",0);
        Assert.Equal(new[] { "accent","name-label","title-label","plate" },GraphicsTemplateEditor.Layers(moved).Select(l=>l.Id));
        var back=GraphicsTemplateEditor.MoveTo(moved,"plate","",99); // clamped to backmost
        Assert.Equal(new[] { "plate","accent","name-label","title-label" },GraphicsTemplateEditor.Layers(back).Select(l=>l.Id));
        Assert.Equal((0d,0d),Pos(moved,"plate"));
    }
    [Fact]
    public void AddLayerUsesReadableIdsLabelsAndBrandTokens()
    {
        var (one,t1)=GraphicsTemplateEditor.AddLayer(Seed,"text"); var (two,t2)=GraphicsTemplateEditor.AddLayer(one,"text");
        Assert.Equal(("text-1","text-2"),(t1,t2)); Assert.Equal("text-2",GraphicsTemplateEditor.Layers(two).Last().Id);
        Assert.Equal("Text",GraphicsTemplateEditor.Layers(two).Last().Label);
        var text=GraphicsTemplateEditor.Layer(two,t2);
        Assert.Equal("brand.fontBody",text.GetProperty("font").GetProperty("token").GetString());
        Assert.Equal((40d,40d,600d,72d),Box(two,t2)); Assert.Equal(42,text.GetProperty("fontSize").GetInt32());
        var (shaped,s1)=GraphicsTemplateEditor.AddLayer(Seed,"shape");
        Assert.Equal("shape-1",s1); Assert.Equal("brand.accent",GraphicsTemplateEditor.Layer(shaped,s1).GetProperty("fill").GetProperty("token").GetString());
        Assert.Equal("rect",GraphicsTemplateEditor.Layer(shaped,s1).GetProperty("type").GetString());
        Assert.Equal("Shape",GraphicsTemplateEditor.Layers(shaped).Last().Label);

        var manifest=SeedObject(); manifest["brandTokens"]=new JsonArray("brand.primary","brand.onPrimary","brand.fontBody");
        var (tokened,_)=GraphicsTemplateEditor.AddLayer(From(manifest),"shape");
        Assert.Contains("brand.accent",Root(tokened).GetProperty("brandTokens").EnumerateArray().Select(t=>t.GetString()));
    }
    [Fact]
    public void AddClockNeedsAClockDesign()
    {
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.AddLayer(Seed,"clock"));
        Assert.Equal("Clocks tick in clock and countdown designs. Start one from the Clock look.",ex.Message);
        var manifest=SeedObject(); manifest["family"]="clock";
        var (clocked,id)=GraphicsTemplateEditor.AddLayer(From(manifest),"clock");
        Assert.Equal("clock-1",id);
        var time=GraphicsTemplateEditor.TemplateFields(clocked).Single(f=>f.Id=="time");
        Assert.Equal("string",time.Type); Assert.Equal(32,time.MaxLength); Assert.Equal("12:00:00",time.Default!.Value.GetString());
        var layer=GraphicsTemplateEditor.Layer(clocked,id);
        Assert.Equal("time",layer.GetProperty("text").GetProperty("field").GetString());
        Assert.Equal((320d,72d),(Num(layer,"width"),Num(layer,"height")));
        Assert.Equal("Clock",GraphicsTemplateEditor.Layers(clocked).Last().Label);
        var (second,id2)=GraphicsTemplateEditor.AddLayer(clocked,"clock");
        Assert.Equal("clock-2",id2); Assert.Single(GraphicsTemplateEditor.TemplateFields(second),f=>f.Id=="time");
    }
    [Fact]
    public void AddImageOffersOnlyExistingSlots()
    {
        var (added,id)=GraphicsTemplateEditor.AddImage(Seed,"new-variable");
        Assert.Equal("image-1",id);
        var field=GraphicsTemplateEditor.TemplateFields(added).Single(f=>f.Id=="image"); Assert.Equal("image",field.Type); Assert.False(field.Required);
        var layer=GraphicsTemplateEditor.Layer(added,id);
        Assert.Equal("image",layer.GetProperty("asset").GetProperty("field").GetString());
        Assert.True(layer.GetProperty("collapseWhenEmpty").GetBoolean());
        Assert.Equal("center",layer.GetProperty("verticalAlign").GetString());
        Assert.Equal((160d,160d),(Num(layer,"width"),Num(layer,"height")));
        Assert.Equal("Image",GraphicsTemplateEditor.Layers(added).Last().Label);
        var (again,id2)=GraphicsTemplateEditor.AddImage(added,"new-variable");
        Assert.Equal("image-2",id2); Assert.Contains(GraphicsTemplateEditor.TemplateFields(again),f=>f.Id=="image-2");
        var (reused,id3)=GraphicsTemplateEditor.AddImage(added,"field:image");
        Assert.Equal("image-2",id3); Assert.Single(GraphicsTemplateEditor.TemplateFields(reused),f=>f.Type=="image");
        var ex=Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddImage(Seed,"asset:missing"));
        Assert.Equal("Choose an image asset this design declares.",ex.Message);
    }
    [Fact]
    public void ImageVerticalAlignIsEditable()
    {
        var (added,id)=GraphicsTemplateEditor.AddImage(Seed,"new-variable");
        var top=GraphicsTemplateEditor.Set(added,id,"verticalAlign",JsonSerializer.SerializeToElement("top"));
        Assert.Equal("top",GraphicsTemplateEditor.Layer(top,id).GetProperty("verticalAlign").GetString());
    }
    [Fact]
    public void DuplicateLabelsTheCopy()
    {
        var copy=GraphicsTemplateEditor.Duplicate(Seed,"plate",out var id);
        Assert.NotEqual("plate",id);
        Assert.Equal("Plate copy",GraphicsTemplateEditor.Layers(copy).Single(l=>l.Id==id).Label);
    }

    // ---- Fix round 1 (task-2 review) ----
    private static void AssertNativeGeometry(NativeGraphicsScratch s)
    {
        void Visit(JsonElement layers) { foreach(var layer in layers.EnumerateArray()) {
            foreach(var key in new[] { "x","y" }) if(layer.TryGetProperty(key,out var v) && v.ValueKind==JsonValueKind.Number) Assert.InRange(v.GetDouble(),0,8192);
            if(layer.TryGetProperty("children",out var c)) Visit(c); } }
        Visit(Root(s).GetProperty("layers"));
    }
    [Fact]
    public void MoveIntoANestedGroupNeverWritesANegativePosition()
    {
        var (grouped,inner)=GraphicsTemplateEditor.Group(Seed,"title-label");
        var (nested,_)=GraphicsTemplateEditor.Group(grouped,inner); // outer at (32,94), inner at (0,0) inside it
        var moved=GraphicsTemplateEditor.MoveTo(nested,"accent",inner,0);
        AssertNativeGeometry(moved);
        Assert.Equal((0d,0d),Pos(moved,inner)); Assert.Equal((0d,0d),Pos(moved,"title-label")); Assert.Equal((0d,0d),Pos(moved,"accent"));
    }
    [Fact]
    public void MoveIntoAPaddedGroupNeverWritesANegativePositionAndKeepsSiblingsInPlace()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        grouped=GraphicsTemplateEditor.Set(grouped,g,"padding",JsonSerializer.SerializeToElement(16));
        var moved=GraphicsTemplateEditor.MoveTo(grouped,"accent",g,0);
        AssertNativeGeometry(moved);
        Assert.Equal((0d,0d),Pos(moved,g)); Assert.Equal((32d,94d),Pos(moved,"title-label")); // title-label still at canvas (48,110)
    }
    [Fact]
    public void MoveFromEarlierInTheSameFlowParentUsesTheOriginAfterRemoval()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Corporate,"title-text"); // group-1 is flow index 1 of text-stack
        var moved=GraphicsTemplateEditor.MoveTo(grouped,"name-text",g,0);
        AssertNativeGeometry(moved);
        Assert.Equal((0d,0d),Pos(moved,"name-text")); // group-1 is now flow index 0 at canvas (24,20): name-text stays at (24,20)
        Assert.False(GraphicsTemplateEditor.Layer(moved,g).TryGetProperty("y",out _));
        Assert.Equal(62d,Box(moved,g).Item4);
    }
    [Fact]
    public void GrowingAPaddedGroupKeepsChildrenInsideItsContentBox()
    {
        var (grouped,g)=GraphicsTemplateEditor.Group(Seed,"title-label");
        grouped=GraphicsTemplateEditor.Set(grouped,g,"padding",JsonSerializer.SerializeToElement(16));
        var moved=GraphicsTemplateEditor.MoveTo(grouped,"name-label",g,0);
        Assert.Equal((16d,11d,1044d,157d),Box(moved,g));
        Assert.Equal((0d,0d),Pos(moved,"name-label")); Assert.Equal((16d,83d),Pos(moved,"title-label"));
    }
    [Fact]
    public void UngroupCarriesAVariableOpacityOrRefuses()
    {
        var manifest=SeedObject(); manifest["fields"]!.AsArray().Add(new JsonObject { ["id"]="fade",["type"]="number",["default"]=1,["bindable"]=true });
        var (grouped,g)=GraphicsTemplateEditor.Group(From(manifest),"accent");
        grouped=GraphicsTemplateEditor.BindLayer(grouped,g,"opacity","fade",null);
        var flat=GraphicsTemplateEditor.Ungroup(grouped,g);
        Assert.Equal("fade",GraphicsTemplateEditor.Layer(flat,"accent").GetProperty("opacity").GetProperty("field").GetString());
        var own=GraphicsTemplateEditor.Set(grouped,"accent","opacity",JsonSerializer.SerializeToElement(0.5));
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Ungroup(own,g));
        Assert.Equal("This group's opacity follows the variable fade. Clear it before ungrouping.",ex.Message);
    }
    [Fact]
    public void UngroupRefusesTwoVariablesDrivingOneLayersVisibility()
    {
        var manifest=SeedObject(); var fields=manifest["fields"]!.AsArray();
        fields.Add(new JsonObject { ["id"]="show-title",["type"]="boolean",["default"]=true,["bindable"]=true });
        fields.Add(new JsonObject { ["id"]="show-block",["type"]="boolean",["default"]=true,["bindable"]=true });
        manifest["layers"]!.AsArray().OfType<JsonObject>().Single(l=>l["id"]!.GetValue<string>()=="title-label")["visible"]=new JsonObject { ["field"]="show-title" };
        var (grouped,g)=GraphicsTemplateEditor.Group(From(manifest),"title-label");
        var doc=JsonNode.Parse(grouped.Manifest)!.AsObject();
        doc["layers"]!.AsArray().OfType<JsonObject>().Single(l=>l["id"]!.GetValue<string>()==g)["visible"]=new JsonObject { ["field"]="show-block" };
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Ungroup(From(doc),g));
        Assert.Equal("Two variables would drive this layer's visibility. Clear one before ungrouping.",ex.Message);
    }
    [Fact]
    public void MovingALayerWhosePositionFollowsAVariableIsRefused()
    {
        var manifest=SeedObject(); manifest["fields"]!.AsArray().Add(new JsonObject { ["id"]="left",["type"]="number",["default"]=4,["bindable"]=true });
        manifest["layers"]!.AsArray().OfType<JsonObject>().Single(l=>l["id"]!.GetValue<string>()=="accent")["x"]=new JsonObject { ["field"]="left" };
        var (grouped,g)=GraphicsTemplateEditor.Group(From(manifest),"title-label");
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.MoveTo(grouped,"accent",g,0));
        Assert.Equal("This layer's position follows a variable. Fix its position before moving it into another group.",ex.Message);
        Assert.NotNull(GraphicsTemplateEditor.MoveTo(grouped,"accent","",0)); // reorder in place keeps the binding
    }
    [Fact]
    public void DuplicateNeverSplitsASurrogatePairInTheCopyName()
    {
        var label=new string('a',58)+"\U0001F600"+"bbb"; // emoji at UTF-16 index 58..59
        var copy=GraphicsTemplateEditor.Duplicate(GraphicsTemplateEditor.SetLabel(Seed,"plate",label),"plate",out var id);
        var name=GraphicsTemplateEditor.Layers(copy).Single(l=>l.Id==id).Label;
        Assert.Equal(new string('a',58)+" copy",name);
    }

    [Fact]
    public void LayersWithoutANameReadAsWords()
    {
        Assert.Equal("Name label",GraphicsTemplateEditor.HumanizeId("name-label"));
        Assert.Equal("Lower third bg",GraphicsTemplateEditor.HumanizeId("lower_third-bg"));
        Assert.Equal("Plate",GraphicsTemplateEditor.HumanizeId("plate"));
        var corporate=GraphicsTemplateEditor.Layers(Corporate);
        Assert.All(corporate,l=>Assert.DoesNotContain("-",l.Label));
    }
    [Fact]
    public void SeedLayersCarryTheMockupNames()
    {
        Assert.Equal(new[] { "Plate","Accent bar","Name","Title" },GraphicsTemplateEditor.Layers(Seed).Select(l=>l.Label));
        foreach(var kind in GraphicsDesignSeeds.Kinds)
            Assert.All(GraphicsTemplateEditor.Layers(From(GraphicsDesignSeeds.Manifest(kind,"design-test-a1b2c3",1,"Test"))),l=>Assert.NotEqual(l.Id,l.Label));
    }
}
