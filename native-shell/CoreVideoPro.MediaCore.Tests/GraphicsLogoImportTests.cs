using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsLogoImportTests : IDisposable
{
    private readonly string _root=Path.Combine(Path.GetTempPath(),"corevideo-logo-"+Guid.NewGuid().ToString("N"));
    private static NativeGraphicsTemplate Logo => new() { TemplateId="logo-accent",TemplateVersion=1,Runtime="declarative",
        Definition=JsonSerializer.SerializeToElement(new {assets=new[] {new {id="logo"}}}),
        Fields=[new() {Id="logo",Type="image",Default=JsonSerializer.SerializeToElement("logo")},new() {Id="show-logo",Type="boolean",Default=JsonSerializer.SerializeToElement(false)}] };

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task UploadAppliesOnlyToUnchangedPrivateDraftAndNeverSendsCueOrShow(bool editDuringImport)
    {
        Directory.CreateDirectory(_root);var path=Path.Combine(_root,"logo.png");
        await File.WriteAllBytesAsync(path,Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg=="));
        var state=new NativeGraphicsState {CoreEpoch="logo-test",Templates=[Logo]};
        var sends=new List<string>();GraphicsWorkspaceSession? session=null;
        session=new((action,body,_)=>{
            sends.Add(action);
            if(action=="graphics.template.install") {
                using var request=JsonDocument.Parse(body);using var manifest=JsonDocument.Parse(request.RootElement.GetProperty("manifest").GetString()!);
                var id=manifest.RootElement.GetProperty("templateId").GetString()!;
                state=state with {Templates=[Logo,new() {TemplateId=id,TemplateVersion=1,Runtime="declarative"}]};
                if(editDuringImport) session!.SetField("show-logo",JsonSerializer.SerializeToElement(true));
            }
            return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true});
        },()=>state,Path.Combine(_root,"assets-cache"));
        session.Observe(state);session.Browse(Logo);var before=session.Draft!;
        var applied=await session.ImportLogoAsync(path);
        Assert.Equal(!editDuringImport,applied);
        Assert.All(sends,action=>Assert.StartsWith("graphics.template.",action));
        Assert.Empty(state.Instances);Assert.Equal("",session.SelectedId);
        if(editDuringImport) {Assert.Null(session.Draft!.Image);Assert.True(session.Draft.Fields.GetProperty("show-logo").GetBoolean());}
        else {Assert.NotNull(session.Draft!.Image);Assert.True(session.Draft.Fields.GetProperty("show-logo").GetBoolean());Assert.Equal("logo",session.Draft.Fields.GetProperty("logo").GetString());session.Undo();Assert.Null(session.Draft!.Image);Assert.False(session.Draft.Fields.GetProperty("show-logo").GetBoolean());}
    }

    [Fact]
    public async Task MissingFileAndNonLogoTemplateKeepPriorDraft()
    {
        var state=new NativeGraphicsState {CoreEpoch="logo-test",Templates=[Logo]};var sends=0;
        var session=new GraphicsWorkspaceSession((_,_,_)=>{sends++;return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true});},()=>state,Path.Combine(_root,"assets-cache"));
        session.Observe(state);session.Browse(Logo);var before=session.Draft;
        Assert.False(await session.ImportLogoAsync(Path.Combine(_root,"absent.png")));Assert.Same(before,session.Draft);Assert.Equal(0,sends);
        Assert.False(GraphicsWorkspaceSession.SupportsLogo(Logo with {Definition=JsonSerializer.SerializeToElement(new {assets=new[] {new {id="photograph"}}})}));
        Assert.True(GraphicsWorkspaceSession.SupportsLogo(new() {TemplateId="compact-solid",Runtime="compatibility"}));
    }
    [Fact]
    public async Task ShowUsesCurrentNativePreparationWithoutWaitingForUiObservation()
    {
        var selected=new NativeGraphicsInstance {InstanceId="logo",TemplateId="corner-logo",TemplateVersion=1,Revision=2,CueRevision=0};
        var state=new NativeGraphicsState {CoreEpoch="logo-test",Instances=[selected]};
        JsonElement request=default;
        var session=new GraphicsWorkspaceSession((action,body,_)=>{
            Assert.Equal("graphics.instance.trigger",action);request=JsonDocument.Parse(body).RootElement.Clone();
            return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true});
        },()=>state);
        session.Observe(state);session.Select("logo");
        state=state with {Instances=[selected with {CueRevision=2}]}; // native ready before the next UI observation
        Assert.True(await session.ShowAsync());
        Assert.Equal(2,request.GetProperty("expectedRevision").GetInt64());
        Assert.Equal("show",request.GetProperty("action").GetString());
    }
    public void Dispose() {if(Directory.Exists(_root)) Directory.Delete(_root,true);}
}
