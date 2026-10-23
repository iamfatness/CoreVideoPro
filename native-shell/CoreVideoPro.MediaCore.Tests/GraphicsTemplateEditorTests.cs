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
}
