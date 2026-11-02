using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsTemplateEditorTests
{
    private static JsonElement Document=>JsonDocument.Parse("""
    {"schemaVersion":1,"templateId":"card","templateVersion":1,"name":"Card","family":"title","canvas":{"width":1920,"height":1080},"bounds":{"width":640,"height":200},"brandTokens":["brand.fontBody"],"fields":[{"id":"name","type":"string","default":"Ada","required":true}],"layers":[{"id":"plate","type":"rect","x":0,"y":0,"width":640,"height":200,"fill":"#102030","metadata":{"publisher":"keep"}},{"id":"group","type":"group","x":20,"y":20,"width":600,"height":160,"children":[{"id":"heading","type":"text","text":{"field":"name"},"font":{"token":"brand.fontBody"},"fontSize":42,"color":"#FFFFFF","width":600,"height":100}]}],"animations":{"in":{"type":"none"},"out":{"type":"none"}},"metadata":{"future":[1,2,3]}}
    """).RootElement.Clone();
    [Fact]
    public void EditsAreImmutablePreserveMetadataAndIdenticalValuesAreNoOps()
    {
        var original=NativeGraphicsScratch.From(Document);
        var same=GraphicsTemplateEditor.Set(original,"plate","width",JsonSerializer.SerializeToElement(640)); Assert.Same(original,same);
        var edited=GraphicsTemplateEditor.Set(original,"plate","width",JsonSerializer.SerializeToElement(800)); Assert.NotEqual(original.Hash,edited.Hash);
        Assert.Equal(640,GraphicsTemplateEditor.Layer(original,"plate").GetProperty("width").GetInt32());
        Assert.Equal("keep",GraphicsTemplateEditor.Layer(edited,"plate").GetProperty("metadata").GetProperty("publisher").GetString());
        Assert.Equal(3,JsonDocument.Parse(edited.Manifest).RootElement.GetProperty("metadata").GetProperty("future").GetArrayLength());
    }
    [Fact]
    public void ParentLocksBlockEditsAndDuplicationKeepsUniqueIdsAndFieldBindings()
    {
        var original=NativeGraphicsScratch.From(Document); var locked=GraphicsTemplateEditor.Lock(original,"group",true);
        Assert.True(GraphicsTemplateEditor.Layers(locked).Single(l=>l.Id=="heading").Locked);
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Set(locked,"heading","x",JsonSerializer.SerializeToElement(5)));
        var duplicate=GraphicsTemplateEditor.Duplicate(original,"group");var layers=GraphicsTemplateEditor.Layers(duplicate);
        Assert.Equal(5,layers.Count);Assert.Equal(5,layers.Select(l=>l.Id).Distinct().Count());
        var copiedText=layers.Last(l=>l.Type=="text"); Assert.Equal("name",GraphicsTemplateEditor.Layer(duplicate,copiedText.Id).GetProperty("text").GetProperty("field").GetString());
        var moved=GraphicsTemplateEditor.Move(original,"group",-1);Assert.Equal("group",GraphicsTemplateEditor.Layers(moved)[0].Id);
    }
    [Fact]
    public void NativePrimitiveCreationAndReparentingPreserveBoundedAcyclicGroups()
    {
        var original=NativeGraphicsScratch.From(Document);var added=GraphicsTemplateEditor.Add(original,"text");
        var text=GraphicsTemplateEditor.Layers(added).Last();Assert.Equal("text",text.Type);
        var nested=GraphicsTemplateEditor.Reparent(added,text.Id,"group");Assert.Equal("group",GraphicsTemplateEditor.Layers(nested).Single(l=>l.Id==text.Id).Parent);
        var grouped=GraphicsTemplateEditor.Add(nested,"group");var group=GraphicsTemplateEditor.Layers(grouped).Last(l=>l.Type=="group");
        grouped=GraphicsTemplateEditor.Reparent(grouped,group.Id,"group");
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Reparent(grouped,"group",group.Id));
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Add(original,"image"));
        Assert.Equal(3,GraphicsTemplateEditor.Layers(original).Count);
    }
    [Fact]
    public void ImageSelectorCanChangeAssetBindingPrivatelyAndStillHonorsLocks()
    {
        var document=JsonNode.Parse(Document.GetRawText())!.AsObject();
        document["assets"]=new JsonArray(new JsonObject { ["id"]="logo",["file"]="assets/logo.png" });
        var original=GraphicsTemplateEditor.Add(NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(document)),"image");
        var id=GraphicsTemplateEditor.Layers(original).Last().Id;
        var changed=GraphicsTemplateEditor.Set(original,id,"asset",JsonSerializer.SerializeToElement(new {token="brand.logo"}));
        Assert.Equal("logo",GraphicsTemplateEditor.Layer(original,id).GetProperty("asset").GetString());
        Assert.Equal("brand.logo",GraphicsTemplateEditor.Layer(changed,id).GetProperty("asset").GetProperty("token").GetString());
        Assert.Contains("brand.logo",JsonDocument.Parse(changed.Manifest).RootElement.GetProperty("brandTokens").EnumerateArray().Select(t=>t.GetString()));
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Set(GraphicsTemplateEditor.Lock(changed,id,true),id,"asset",JsonSerializer.SerializeToElement("logo")));
    }
    private static NativeGraphicsState State()
    {
        var doc=Document;return new() {SchemaVersion=1,CoreEpoch="test-core",Templates=[new() {TemplateId="card",TemplateVersion=1,Runtime="declarative",Family="title",Definition=doc,ManifestHash=NativeGraphicsScratch.From(doc).Hash,Fields=[new() {Id="name",Type="string",Default=JsonSerializer.SerializeToElement("Ada"),Required=true}]}],
            Instances=[new() {InstanceId="featured",TemplateId="card",TemplateVersion=1,ProgramTemplateVersion=1,Revision=3,ProgramRevision=3,Fields=new() {Name="Ada"},ProgramFields=new() {Name="Ada"}}]};
    }
    [Fact]
    public async Task ScratchEditsAndUndoArePrivateAndCannotBeAccidentallyCued()
    {
        var state=State();var sent=0;var session=new GraphicsWorkspaceSession((_,_,_)=>{sent++;return Task.FromResult(new NativeGraphicsOperationResult());},()=>state);
        session.Observe(state);session.Select("featured");session.EditTemplate(d=>GraphicsTemplateEditor.Set(d,"plate","fill",JsonSerializer.SerializeToElement("#FF0000")));
        Assert.NotNull(session.Preview!.ForPreview().Scratch);Assert.Equal(3,session.Selected!.ProgramRevision);
        Assert.False(await session.CueAsync());Assert.Equal(0,sent);Assert.Contains("Save",session.Status);
        session.Undo();Assert.Null(session.Draft!.Scratch);Assert.Equal(0,sent);
    }
    [Fact]
    public void CanvasAcceptsOnlyNativeLayoutForTheCurrentPrivateValues()
    {
        var state=State();var fields=JsonSerializer.SerializeToElement(new {name="Ada"});
        state=state with {Authoring=new() {Key="first",State="validated",Input=new() {TemplateId="card",TemplateVersion=1,ManifestHash=state.Templates[0].ManifestHash,Fields=fields},Layout=new() {Width=640,Height=200,Nodes=[new() {Id="heading",X=20,Y=20,Width=600,Height=100,Visible=true}]}}};
        var session=new GraphicsWorkspaceSession((_,_,_)=>throw new InvalidOperationException(),()=>state);session.Observe(state);session.Select("featured");Assert.NotNull(session.AuthoringLayout);
        session.SetField("name",JsonSerializer.SerializeToElement("Edited"));Assert.Null(session.AuthoringLayout);
        state=state with {Authoring=state.Authoring with {Key="second",Input=state.Authoring.Input! with {Fields=JsonSerializer.SerializeToElement(new {name="Edited"})}}};session.Observe(state);Assert.NotNull(session.AuthoringLayout);
        session.EditTemplate(d=>GraphicsTemplateEditor.Set(d,"plate","width",JsonSerializer.SerializeToElement(800)));Assert.Null(session.AuthoringLayout);
    }
    [Theory]
    [InlineData(false)] [InlineData(true)]
    public async Task SavePublishesExactVersionWithoutApplyingAndRejectsConflictingImports(bool conflict)
    {
        var state=State();var actions=new List<string>();JsonElement manifest=default;
        var session=new GraphicsWorkspaceSession((action,request,_)=>{
            actions.Add(action);var body=JsonDocument.Parse(request).RootElement;
            if(action=="graphics.template.install") manifest=JsonDocument.Parse(body.GetProperty("manifest").GetString()!).RootElement.Clone();
            if(action=="graphics.template.list") {
                if(conflict) {var other=JsonNode.Parse(manifest.GetRawText())!;other["name"]="Other import";manifest=JsonSerializer.SerializeToElement(other);}
                state=state with {Templates=[state.Templates[0],state.Templates[0] with {TemplateVersion=2,Definition=manifest,ManifestHash=NativeGraphicsScratch.From(manifest).Hash}]};
            }
            return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true,InstanceId="featured",CurrentRevision=3});
        },()=>state);
        session.Observe(state);session.Select("featured");session.EditTemplate(d=>GraphicsTemplateEditor.Set(d,"plate","fill",JsonSerializer.SerializeToElement("#FF0000")));
        Assert.Equal(!conflict,await session.SaveTemplateVersionAsync());
        Assert.DoesNotContain("graphics.instance.update",actions);Assert.DoesNotContain("graphics.instance.trigger",actions);Assert.Equal(3,session.Selected!.ProgramRevision);
        Assert.Equal(conflict?1:2,session.Draft!.TemplateVersion);Assert.Equal(conflict,session.Draft.Scratch is not null);
    }

    private static NativeGraphicsScratch LowerThird()=>NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest("lower-third","design-vars-a1b2c3",1,"Vars")));
    private static JsonObject Parse(NativeGraphicsScratch s)=>JsonNode.Parse(s.Manifest)!.AsObject();
    private static JsonObject FieldOf(NativeGraphicsScratch s,string id)=>Parse(s)["fields"]!.AsArray().OfType<JsonObject>().Single(f=>f["id"]!.GetValue<string>()==id);
    private static JsonElement Json(object value)=>JsonSerializer.SerializeToElement(value);

    [Fact]
    public void AddFieldMapsKindsToNativeTypesAndBindsANewTextLayer()
    {
        var s=GraphicsTemplateEditor.AddField(LowerThird(),"location","text",40);
        var f=FieldOf(s,"location"); Assert.Equal("string",f["type"]!.GetValue<string>()); Assert.Equal(40,f["maxLength"]!.GetValue<int>());
        foreach(var (kind,native) in new[]{("number","number"),("colour","color"),("on-off","boolean"),("image","image")})
            Assert.Equal(native,FieldOf(GraphicsTemplateEditor.AddField(s,"extra-"+kind,kind),"extra-"+kind)["type"]!.GetValue<string>());
        s=GraphicsTemplateEditor.Add(s,"text"); var layer=GraphicsTemplateEditor.Layers(s).Last().Id;
        s=GraphicsTemplateEditor.BindLayer(s,layer,"text","location",null);
        Assert.Equal("location",GraphicsTemplateEditor.Layer(s,layer).GetProperty("text").GetProperty("field").GetString());
        Assert.Contains(GraphicsTemplateEditor.FieldUsage(s),u=>u.FieldId=="location" && u.LayerLabels.SequenceEqual(new[]{layer}));
        Assert.Contains(GraphicsTemplateEditor.FieldUsage(s),u=>u.FieldId=="name" && u.LayerLabels.SequenceEqual(new[]{"name-label"}));
    }
    [Fact]
    public void RenameFieldRewritesEveryBindingAndRefusesCollisionsAndBadIds()
    {
        var s=GraphicsTemplateEditor.AddField(LowerThird(),"location","text",40);
        s=GraphicsTemplateEditor.Add(s,"text"); var layer=GraphicsTemplateEditor.Layers(s).Last().Id;
        s=GraphicsTemplateEditor.BindLayer(s,layer,"text","location",null);
        s=GraphicsTemplateEditor.Duplicate(s,layer);
        var renamed=GraphicsTemplateEditor.RenameField(s,"location","city");
        Assert.DoesNotContain("\"location\"",renamed.Manifest);
        Assert.Equal(2,GraphicsTemplateEditor.FieldUsage(renamed).Single(u=>u.FieldId=="city").LayerLabels.Count);
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.RenameField(s,"location","name"));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.RenameField(s,"location","Bad Id"));
        Assert.Same(s,GraphicsTemplateEditor.RenameField(s,"location","location"));
    }
    [Fact]
    public void RemoveFieldRefusesUsedFieldNamingLayersButRemovesUnusedOnes()
    {
        var s=LowerThird();
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.RemoveField(s,"title"));
        Assert.Equal("Used by: title-label",ex.Message);
        s=GraphicsTemplateEditor.AddField(s,"spare","text");
        var gone=GraphicsTemplateEditor.RemoveField(s,"spare");
        Assert.DoesNotContain(Parse(gone)["fields"]!.AsArray(),f=>f!["id"]!.GetValue<string>()=="spare");
    }
    [Fact]
    public void FieldLimitsAndIdRulesFollowNative()
    {
        var s=LowerThird();
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddField(s,"Upper","text"));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddField(s,"1st","text"));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddField(s,new string('a',65),"text"));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddField(s,"ok","bogus"));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddField(s,"ok","number",10));
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.AddField(s,"name","text"));
        _=GraphicsTemplateEditor.AddField(s,new string('a',64),"text");
        for(var i=0;i<30;i++) s=GraphicsTemplateEditor.AddField(s,"f"+i,"text");
        Assert.Equal(32,Parse(s)["fields"]!.AsArray().Count);
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.AddField(s,"one-too-many","text"));
    }
    [Fact]
    public void HideWhenEmptySetsCollapseOnBoundTextLayersAndOptionsAreValidated()
    {
        var s=LowerThird();
        var on=GraphicsTemplateEditor.SetFieldOptions(s,"name",64,Json("Grace"),true);
        Assert.True(GraphicsTemplateEditor.Layer(on,"name-label").GetProperty("collapseWhenEmpty").GetBoolean());
        Assert.Equal(64,FieldOf(on,"name")["maxLength"]!.GetValue<int>()); Assert.Equal("Grace",FieldOf(on,"name")["default"]!.GetValue<string>());
        var off=GraphicsTemplateEditor.SetFieldOptions(on,"name",null,null,false);
        Assert.False(GraphicsTemplateEditor.Layer(off,"name-label").TryGetProperty("collapseWhenEmpty",out _)); Assert.Null(FieldOf(off,"name")["maxLength"]);
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetFieldOptions(s,"name",3,Json("Grace"),false));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetFieldOptions(s,"name",null,Json(5),false));
        var c=GraphicsTemplateEditor.AddField(s,"tint","colour");
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetFieldOptions(c,"tint",null,Json("red"),false));
        Assert.Equal("#102030",FieldOf(GraphicsTemplateEditor.SetFieldOptions(c,"tint",null,Json("#102030"),false),"tint")["default"]!.GetValue<string>());
    }
    [Fact]
    public void BindLayerNeedsExactlyOneSourceAndMatchingFieldTypeAndRespectsLocks()
    {
        var s=GraphicsTemplateEditor.AddField(GraphicsTemplateEditor.AddField(LowerThird(),"tint","colour"),"count","number");
        Assert.Throws<ArgumentException>(()=>GraphicsTemplateEditor.BindLayer(s,"plate","fill",null,null));
        Assert.Throws<ArgumentException>(()=>GraphicsTemplateEditor.BindLayer(s,"plate","fill","tint",Json("#FFFFFF")));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"plate","fill","count",null));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"plate","fill","missing",null));
        var bound=GraphicsTemplateEditor.BindLayer(s,"plate","fill","tint",null);
        Assert.Equal("tint",GraphicsTemplateEditor.Layer(bound,"plate").GetProperty("fill").GetProperty("field").GetString());
        var brand=GraphicsTemplateEditor.BindLayer(bound,"plate","fill",null,Json(new{token="brand.accent"}));
        Assert.Equal("brand.accent",GraphicsTemplateEditor.Layer(brand,"plate").GetProperty("fill").GetProperty("token").GetString());
        Assert.Empty(GraphicsTemplateEditor.FieldUsage(brand).Single(u=>u.FieldId=="tint").LayerLabels);
        Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.BindLayer(GraphicsTemplateEditor.Lock(s,"plate",true),"plate","fill","tint",null));
    }
    [Fact]
    public void EditedManifestKeepsNativeShape()
    {
        // No C# scratch-validation helper exists; native loading is proven by the native seed test. Assert structure.
        var s=GraphicsTemplateEditor.AddField(LowerThird(),"location","text",40);
        s=GraphicsTemplateEditor.Add(s,"text"); var layer=GraphicsTemplateEditor.Layers(s).Last().Id;
        s=GraphicsTemplateEditor.SetFieldOptions(GraphicsTemplateEditor.BindLayer(s,layer,"text","location",null),"location",40,Json("Oslo"),true);
        var root=Parse(s); var ids=root["fields"]!.AsArray().Select(f=>f!["id"]!.GetValue<string>()).ToArray();
        Assert.Equal(ids.Length,ids.Distinct().Count());
        foreach(var f in root["fields"]!.AsArray()) Assert.Contains(f!["type"]!.GetValue<string>(),new[]{"string","number","color","boolean","image"});
        var node=root["layers"]!.AsArray().OfType<JsonObject>().Single(l=>l["id"]!.GetValue<string>()==layer);
        Assert.Equal("location",node["text"]!["field"]!.GetValue<string>()); Assert.True(node["collapseWhenEmpty"]!.GetValue<bool>());
        Assert.Contains(ids,i=>i=="name"); Assert.Equal("lower-third",root["family"]!.GetValue<string>());
    }

    private static NativeGraphicsScratch WithGradient(NativeGraphicsScratch s)
    {
        s=GraphicsTemplateEditor.AddField(GraphicsTemplateEditor.AddField(s,"from-colour","colour"),"to-colour","colour");
        return GraphicsTemplateEditor.Set(s,"plate","fill",Json(new{stops=new object[]{new{field="from-colour"},new{field="to-colour"}},direction="horizontal"}));
    }
    [Fact]
    public void GradientStopBindingsCountAsUsageAndRenameRewritesThem()
    {
        var s=WithGradient(LowerThird());
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.RemoveField(s,"to-colour"));
        Assert.Equal("Used by: plate",ex.Message);
        Assert.Equal(["plate"],GraphicsTemplateEditor.FieldUsage(s).Single(u=>u.FieldId=="from-colour").LayerLabels);
        var renamed=GraphicsTemplateEditor.RenameField(s,"to-colour","end-colour");
        var stops=GraphicsTemplateEditor.Layer(renamed,"plate").GetProperty("fill").GetProperty("stops");
        Assert.Equal("from-colour",stops[0].GetProperty("field").GetString()); Assert.Equal("end-colour",stops[1].GetProperty("field").GetString());
        Assert.DoesNotContain("to-colour",renamed.Manifest);
    }
    [Fact]
    public void NumberBindingsMustKeepTheSampleInsideTheNativeRange()
    {
        var s=GraphicsTemplateEditor.AddField(LowerThird(),"size","number");
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"name-label","fontSize","size",null)); // default 0, native needs 1-512
        s=GraphicsTemplateEditor.SetFieldOptions(s,"size",null,Json(24),false);
        var bound=GraphicsTemplateEditor.BindLayer(s,"name-label","fontSize","size",null);
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetFieldOptions(bound,"size",null,Json(0),false));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.SetFieldOptions(bound,"size",null,Json(600),false));
        Assert.Equal(48,FieldOf(GraphicsTemplateEditor.SetFieldOptions(bound,"size",null,Json(48),false),"size")["default"]!.GetValue<double>());
        var opacity=GraphicsTemplateEditor.AddField(LowerThird(),"fade","number"); // 0 is valid for opacity
        Assert.NotNull(GraphicsTemplateEditor.BindLayer(opacity,"plate","opacity","fade",null));
    }
    [Fact]
    public void EditsThatWouldOrphanARequiredFieldAreRefusedNamingIt()
    {
        var s=LowerThird(); // name is required and used only by name-label
        var ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.BindLayer(s,"name-label","text",null,Json("Fixed")));
        Assert.Contains("name",ex.Message);
        ex=Assert.Throws<InvalidOperationException>(()=>GraphicsTemplateEditor.Remove(s,"name-label"));
        Assert.Contains("name",ex.Message);
        Assert.NotNull(GraphicsTemplateEditor.Remove(s,"title-label")); // optional field may be orphaned
        var second=GraphicsTemplateEditor.Duplicate(s,"name-label"); // still used elsewhere
        Assert.NotNull(GraphicsTemplateEditor.Remove(second,"name-label"));
    }
    [Fact]
    public void BindLayerRefusesPropertiesTheLayerTypeLacksAndLiteralFieldReferences()
    {
        var s=GraphicsTemplateEditor.AddField(LowerThird(),"extra","text");
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"plate","text","extra",null));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"plate","fontSize",null,Json(20)));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"title-label","text",null,Json(new{field="extra"})));
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.BindLayer(s,"nope","text","extra",null));
    }
    [Fact]
    public void AddFieldChecksItsDefaultAgainstMaxLengthAndHideWhenEmptySkipsLockedLayers()
    {
        Assert.Throws<InvalidDataException>(()=>GraphicsTemplateEditor.AddField(LowerThird(),"tint","colour",3)); // default #FFFFFF is 7 long
        var locked=GraphicsTemplateEditor.Lock(LowerThird(),"title-label",true);
        var result=GraphicsTemplateEditor.SetFieldOptions(locked,"title",null,null,false); // seed has collapse on title-label
        Assert.True(GraphicsTemplateEditor.Layer(result,"title-label").GetProperty("collapseWhenEmpty").GetBoolean());
    }
    [Fact]
    public void VariablesListsKindLimitSampleIfEmptyAndUsedBy()
    {
        var s=GraphicsTemplateEditor.AddField(LowerThird(),"show-logo","on-off");
        var rows=GraphicsTemplateEditor.Variables(s);
        Assert.Equal(new[]{"name","title","show-logo"},rows.Select(r=>r.Id));
        var name=rows[0]; Assert.Equal(("text",128,"Name",true,false),(name.Kind,name.MaxLength,name.Sample.GetString(),name.Required,name.HideWhenEmpty));
        Assert.Equal(new[]{"name-label"},name.UsedBy);
        var title=rows[1]; Assert.True(title.HideWhenEmpty); Assert.False(title.Required); Assert.Equal(new[]{"title-label"},title.UsedBy);
        var logo=rows[2]; Assert.Equal(("on-off",(int?)null,false),(logo.Kind,logo.MaxLength,logo.Sample.GetBoolean())); Assert.Empty(logo.UsedBy);
        Assert.False(GraphicsTemplateEditor.Variables(GraphicsTemplateEditor.SetFieldOptions(s,"title",256,null,false))[1].HideWhenEmpty);
    }
    [Fact]
    public void SyncFieldsMatchesTheDraftValuesToTheEditedVariables()
    {
        var current=Json(new{name="Ada",title="Host"});
        var renamed=GraphicsTemplateEditor.AddField(GraphicsTemplateEditor.RenameField(LowerThird(),"title","role"),"location","text",40);
        var synced=GraphicsTemplateEditor.SyncFields(current,renamed,"title","role");
        Assert.Equal(new[]{"name","role","location"},synced.EnumerateObject().Select(p=>p.Name));
        Assert.Equal(("Ada","Host",""),(synced.GetProperty("name").GetString(),synced.GetProperty("role").GetString(),synced.GetProperty("location").GetString()));
        var removed=GraphicsTemplateEditor.SyncFields(synced,GraphicsTemplateEditor.RemoveField(renamed,"location"));
        Assert.False(removed.TryGetProperty("location",out _));
        var capped=GraphicsTemplateEditor.SetFieldOptions(renamed,"name",3,Json("Al"),false);
        Assert.Equal("Al",GraphicsTemplateEditor.SyncFields(Json(new{name="Ada Lovelace",role="Host",location=""}),capped).GetProperty("name").GetString());
    }
    [Fact]
    public void PreviewValidationChecksTheEditedVariablesNotTheInstalledOnes()
    {
        var seed=LowerThird(); var definition=JsonDocument.Parse(seed.Manifest).RootElement.Clone();
        var template=JsonSerializer.Deserialize<NativeGraphicsTemplate>(seed.Manifest,new JsonSerializerOptions(JsonSerializerDefaults.Web))! with {Runtime="declarative",Definition=definition,ManifestHash=seed.Hash};
        var state=new NativeGraphicsState {SchemaVersion=1,CoreEpoch="test-core",Templates=[template]};
        var session=new GraphicsWorkspaceSession((_,_,_)=>Task.FromResult(new NativeGraphicsOperationResult()),()=>state);
        session.Observe(state); session.Browse(template);
        Assert.Equal("",session.PreviewValidation);
        session.EditTemplate(d=>GraphicsTemplateEditor.AddField(d,"location","text",40));
        Assert.StartsWith("location:",session.PreviewValidation);
        session.Edit(session.Draft! with {Fields=GraphicsTemplateEditor.SyncFields(session.Draft.Fields,session.Draft.Scratch!)});
        Assert.Equal("",session.PreviewValidation);
    }
}
