using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsWorkspaceLibraryTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "corevideo-graphics-library-" + Guid.NewGuid().ToString("N"));
    private string DocumentPath => Path.Combine(_directory, "show.json");
    private static NativeGraphicsState State => new() { SchemaVersion = 1, CoreEpoch = "test-core",
        Templates = [new() { TemplateId = "compact-solid", TemplateVersion = 1, Runtime = "compatibility" }],
        Instances = [new() { InstanceId = "featured", TemplateId = "compact-solid", TemplateVersion = 1, Revision = 1, Scope = "global",
            Fields = new() { Name = "Ada", Title = "" }, Style = new() { NameSize = 64 } }] };
    [Fact]
    public async Task LegacyMigrationCopiesExactBlanksStylesAndUnknownBrandWithoutNativeActions()
    {
        var session=new GraphicsWorkspaceSession((_,_,_)=>throw new InvalidOperationException("Migration cannot touch native output"),()=>State);
        session.Observe(State);session.Select("featured");
        var library=new GraphicsWorkspaceLibrary(new(DocumentPath),session);
        var overrides=JsonSerializer.SerializeToElement(new {names=new Dictionary<string,string>{{"zoom:7","Guest"}},secondaryLines=new Dictionary<string,string>{{"zoom:7",""}}});
        var brand=JsonSerializer.SerializeToElement(new {customMetadata="Retain",fontFamily="Segoe UI"});
        Assert.True(await library.ImportLegacyAsync(session.Draft!,"zoom:7",overrides,brand,new Dictionary<string,LowerThirdAppearance>{{"Saved look",new(){NameSize=73}}}));
        Assert.False(await library.ImportLegacyAsync(session.Draft!,"zoom:7",overrides,brand,new Dictionary<string,LowerThirdAppearance>()));
        var saved=await new GraphicsDocumentStore(DocumentPath).LoadAsync();
        Assert.Equal("",saved.Document.SourceOverrides!.Value.GetProperty("secondaryLines").GetProperty("zoom:7").GetString());
        Assert.Equal("Retain",saved.Document.Brand!.Value.GetProperty("customMetadata").GetString());
        Assert.Equal(73,saved.Document.StylePresets["Saved look"].NameSize);
        Assert.Equal("",Assert.Single(saved.Document.Cues).Draft!.Fields.GetProperty("title").GetString());
        Assert.Equal("legacy",saved.Document.Authority);
    }
    [Fact]
    public async Task LoadingSavingAndApplyingStylesArePrivateAndRestoreOnlyCreatesHiddenCue()
    {
        var native = State; var sends = new List<(string Action, JsonElement Body)>();
        var session = new GraphicsWorkspaceSession((action, request, _) => {
            using var json = JsonDocument.Parse(request); var body = json.RootElement.Clone(); sends.Add((action, body));
            if (action == "graphics.instance.create") native = native with { Instances = [..native.Instances,
                native.Instances[0] with { InstanceId = body.GetProperty("instanceId").GetString()!, ProgramRevision = 0 }] };
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true });
        }, () => native);
        session.Observe(native); session.Select("featured");
        var library = new GraphicsWorkspaceLibrary(new(DocumentPath), session); await library.LoadAsync();
        Assert.True(await library.SaveCueAsync("Interview")); Assert.True(await library.SaveStyleAsync("Large"));
        session.SetStyle(session.Draft!.Style with { NameSize = 32 }); library.ApplyStyle("Large"); Assert.Equal(64, session.Draft!.Style.NameSize);
        Assert.Empty(sends);
        var loaded = await new GraphicsDocumentStore(DocumentPath).LoadAsync();
        Assert.Equal("", loaded.Document.Cues[0].Draft!.Fields.GetProperty("title").GetString());
        Assert.True(await library.RestoreAsync(library.Document.Cues[0]));
        Assert.Equal(2,sends.Count);var send = sends[0]; Assert.Equal("graphics.instance.create", send.Action);
        Assert.Equal("graphics.instance.update",sends[1].Action);Assert.Equal("Interview",sends[1].Body.GetProperty("label").GetString());
        Assert.False(send.Body.TryGetProperty("action", out _)); Assert.False(send.Body.TryGetProperty("visibility", out _));
        Assert.NotEqual("featured", send.Body.GetProperty("instanceId").GetString());
    }
    [Fact]
    public async Task RecycledSourceAndDifferentTemplateHashNeverRestoreSilently()
    {
        var sends = 0; var native = State;
        var session = new GraphicsWorkspaceSession((_, _, _) => { sends++; return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true }); }, () => native);
        session.Observe(native); session.Select("featured");
        var library = new GraphicsWorkspaceLibrary(new(DocumentPath), session); await library.LoadAsync();
        var bound = new GraphicsSavedCue { Id = "old", SourceId = "zoom:44", Draft = session.Draft };
        Assert.False(await library.RestoreAsync(bound)); Assert.Equal(0, sends);
        var definition = JsonSerializer.SerializeToElement(new { schemaVersion = 1, templateId = "interview", templateVersion = 2, metadata = new { note = "Preserve" } });
        var document = new GraphicsShowDocument { Templates = [new("interview", 2, new('a', 64), definition)],
            Cues = [new() { Id = "saved", Draft = session.Draft! with { TemplateId = "interview", TemplateVersion = 2 } }] };
        await new GraphicsDocumentStore(DocumentPath).SaveAsync(document);
        native = native with { Templates = [new() { TemplateId = "interview", TemplateVersion = 2, Runtime = "declarative", ManifestHash = new('b', 64) }] };
        session.Observe(native); library = new(new(DocumentPath), session); await library.LoadAsync();
        Assert.False(await library.RestoreAsync(library.Document.Cues[0])); Assert.Equal(0, sends);
        Assert.Contains("differs", library.Status);
        Assert.Equal("Preserve", library.Document.Templates[0].Definition.GetProperty("metadata").GetProperty("note").GetString());
    }
    [Fact]
    public void DeclarativePreviewUsesTheSameConsumedStyleWireAsCommit()
    {
        var draft = new NativeGraphicsDraft("interview", 2, JsonSerializer.SerializeToElement(new { name = "Guest" }), new(), new());
        var preview = JsonSerializer.SerializeToElement(new LowerThirdPreview(null, Graphics: draft.ForPreview()), new JsonSerializerOptions(JsonSerializerDefaults.Web));
        var style = preview.GetProperty("graphics").GetProperty("style");
        Assert.False(style.TryGetProperty("width", out _)); Assert.False(style.TryGetProperty("showLogo", out _));
        Assert.True(style.TryGetProperty("fontFamily", out _)); Assert.True(style.TryGetProperty("safeOffsetX", out _));
    }
    [Fact]
    public async Task RuleSaveRequiresSavedTargetsAndRestoringRulesNeverArmsOrPersistsLiveIdentity()
    {
        var state=State with {Automation=new() {Mode="automatic",Armed=true,Configuration=new() {Rules=[new() {Id="intro",InstanceId="featured"}]}}};
        var commands=new List<(string Action,JsonElement Body)>();
        var session=new GraphicsWorkspaceSession((action,body,_)=>{commands.Add((action,JsonDocument.Parse(body).RootElement.Clone()));return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true});},()=>state);
        session.Observe(state);session.Select("featured");
        var library=new GraphicsWorkspaceLibrary(new(DocumentPath),session);
        Assert.False(await library.SaveAutomationRulesAsync());
        Assert.True(await library.SaveCueAsync("Interview"));Assert.True(await library.SaveAutomationRulesAsync());
        var disk=await File.ReadAllTextAsync(DocumentPath);
        Assert.DoesNotContain("armed",disk,StringComparison.OrdinalIgnoreCase);Assert.DoesNotContain("processEpoch",disk,StringComparison.OrdinalIgnoreCase);
        Assert.True(await library.RestoreAutomationRulesAsync(rules=>new() {Rules=rules}));
        var command=Assert.Single(commands);Assert.Equal("graphics.automation.configure",command.Action);
        Assert.Equal("featured",command.Body.GetProperty("configuration").GetProperty("rules")[0].GetProperty("instanceId").GetString());
        state=state with {CoreEpoch="replacement",Instances=[]};session.Observe(state);
        Assert.False(await library.RestoreAutomationRulesAsync(rules=>new() {Rules=rules}));Assert.Single(commands);
    }
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
