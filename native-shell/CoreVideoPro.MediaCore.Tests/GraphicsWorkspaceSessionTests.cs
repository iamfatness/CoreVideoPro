using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsWorkspaceSessionTests
{
    private static NativeGraphicsState Initial => new() {
        SchemaVersion = 1, CoreEpoch = "core-one", Instances = [new() { InstanceId = "featured", TemplateId = "compact-solid", TemplateVersion = 1,
            Revision = 3, ProgramRevision = 1, CueRevision = 2, Preparation = "ready", Fields = new() { Name = "Private", Title = "" },
            CueFields = new() { Name = "Accepted cue" }, ProgramFields = new() { Name = "On air" } }] };
    private static JsonElement Value(string text) => JsonSerializer.SerializeToElement(text);
    [Fact]
    public async Task BindingEditsRemainPrivateAndStaleGenerationCannotReplaceSubject()
    {
        var identity=new CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity {SourceId="zoom:7",InstanceId="participant",ProcessEpoch="meeting-one",Generation=1};
        var state=Initial with {Sources=[new() {Identity=identity,DisplayName="Ada",VideoOn=true}]};
        var sent=new List<JsonElement>();
        var session=new GraphicsWorkspaceSession((_,request,_)=> {sent.Add(JsonDocument.Parse(request).RootElement.Clone());return Task.FromResult(new NativeGraphicsOperationResult {Accepted=true});},()=>state);
        session.Observe(state);session.Select("featured");session.SetBinding(identity);session.CopySourceText("Ada","");
        Assert.Empty(sent);Assert.Equal(identity,session.Draft!.Binding);
        Assert.Equal("",session.Draft.Fields.GetProperty("title").GetString());
        var persisted=JsonSerializer.SerializeToElement(session.Draft,new JsonSerializerOptions(JsonSerializerDefaults.Web));
        Assert.False(persisted.TryGetProperty("binding",out _));
        session.SetBinding(identity with {Generation=2});Assert.Equal(identity,session.Draft.Binding);
        await session.UpdateProgramAsync();Assert.Equal(1,sent[0].GetProperty("binding").GetProperty("generation").GetInt64());
        session.SetBinding(null);session.Undo();Assert.Equal(identity,session.Draft.Binding);
    }
    [Fact]
    public async Task SampleCopyIsPrivateForCompatibilityLooksAndCannotBeCommitted()
    {
        var sends = 0;
        var session = new GraphicsWorkspaceSession((_,_,_) => { sends++; return Task.FromResult(new NativeGraphicsOperationResult()); }, () => Initial);
        session.Observe(Initial); session.Select("featured");
        session.SetPreviewSample("long");
        Assert.StartsWith("Dr. Alexandra", session.Preview!.Fields.GetProperty("name").GetString());
        Assert.Equal("Private", session.Draft!.Fields.GetProperty("name").GetString());
        Assert.False(await session.CueAsync()); Assert.False(await session.UpdateProgramAsync());
        Assert.Equal(0, sends);
        session.SetPreviewSample("current");
        Assert.Equal("Private", session.Preview.Fields.GetProperty("name").GetString());
    }
    [Fact]
    public void SamplesOnlyReplaceConsumedTextAndRespectUnicodeByteLimits()
    {
        var template = new NativeGraphicsTemplate { Runtime="declarative", Fields=[new() {Id="heading",Type="string",MaxLength=7},new() {Id="font",Type="string"},new() {Id="time",Type="string"}],
            Layers=JsonDocument.Parse("[{\"type\":\"text\",\"text\":{\"field\":\"heading\"}},{\"type\":\"text\",\"text\":{\"field\":\"time\"}}]").RootElement.Clone() };
        var draft = new NativeGraphicsDraft("custom",1,JsonSerializer.SerializeToElement(new {heading="Actual",font="Segoe UI",time="01:02"}),new(),new());
        var result = GraphicsPreviewSamples.Apply(draft,template,"unicode");
        Assert.Equal("Zoë ·",result.GetProperty("heading").GetString());
        Assert.Equal("Segoe UI",result.GetProperty("font").GetString());
        Assert.Equal("01:02",result.GetProperty("time").GetString());
        Assert.Equal("Actual",draft.Fields.GetProperty("heading").GetString());
    }
    [Fact]
    public void DraftCompareUndoAndMetadataRefreshNeverSendCommandsOrLoseEdits()
    {
        var state = Initial; var sends = 0;
        var workspace = new GraphicsWorkspaceSession((_, _, _) => { sends++; return Task.FromResult(new NativeGraphicsOperationResult()); }, () => state);
        workspace.Observe(state); workspace.Select("featured");
        workspace.SetField("name", Value("Edited")); workspace.SetCompare(true);
        Assert.Equal("On air", workspace.Preview!.Fields.GetProperty("name").GetString());
        Assert.Equal("Edited", workspace.Draft!.Fields.GetProperty("name").GetString());
        workspace.Observe(state with { Instances = [state.Instances[0] with { FirstDrawSubmittedFrame = 100, Visibility = "on-air" }] });
        Assert.Equal("Edited", workspace.Draft.Fields.GetProperty("name").GetString());
        workspace.SetCompare(false); workspace.Undo();
        Assert.Equal("Private", workspace.Preview!.Fields.GetProperty("name").GetString());
        Assert.Equal(0, sends);
    }
    [Fact]
    public void WholeGestureIsOneUndoAndEscapeCancelsWithoutChangingSelection()
    {
        var workspace = new GraphicsWorkspaceSession((_, _, _) => throw new InvalidOperationException(), () => Initial);
        workspace.Observe(Initial); workspace.Select("featured");
        workspace.BeginGesture(); workspace.SetStyle(workspace.Draft!.Style with { NameSize = 50 });
        workspace.SetStyle(workspace.Draft.Style with { NameSize = 60 }); workspace.EndGesture(); workspace.Undo();
        Assert.Equal(42, workspace.Draft!.Style.NameSize); Assert.False(workspace.CanUndo);
        workspace.BeginGesture(); workspace.SetStyle(workspace.Draft.Style with { NameSize = 90 }); workspace.CancelGesture();
        Assert.Equal(42, workspace.Draft.Style.NameSize); Assert.Equal("featured", workspace.SelectedId);
    }
    [Fact]
    public async Task ShowUsesAcceptedCueWhileUpdateUsesCurrentDraftAndClearIsExplicit()
    {
        var state = Initial; var sent = new List<(string Action, JsonElement Body)>();
        var workspace = new GraphicsWorkspaceSession((action, request, _) => {
            using var json = JsonDocument.Parse(request); sent.Add((action, json.RootElement.Clone()));
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, Reason = "accepted" });
        }, () => state);
        workspace.Observe(state); workspace.Select("featured"); workspace.SetField("title", Value(""));
        Assert.True(await workspace.ShowAsync());
        Assert.Equal("cue", sent[0].Body.GetProperty("target").GetString()); Assert.Equal(2, sent[0].Body.GetProperty("expectedRevision").GetInt32());
        Assert.True(await workspace.UpdateProgramAsync());
        Assert.Equal("program", sent[1].Body.GetProperty("target").GetString()); Assert.Equal(3, sent[1].Body.GetProperty("expectedRevision").GetInt32());
        Assert.Equal("", sent[1].Body.GetProperty("fields").GetProperty("title").GetString());
        Assert.False(await workspace.RemoveAsync()); Assert.Equal(2, sent.Count);
        Assert.True(await workspace.ClearAsync()); Assert.Equal("all", sent[^1].Body.GetProperty("channel").GetString());
    }
    [Fact]
    public async Task CueWaitsForDraftAdmissionAndUsesReturnedRevisionWithoutShowingAnything()
    {
        var state = Initial; var sent = new List<string>();
        GraphicsWorkspaceSession? workspace = null;
        workspace = new((action, request, _) => {
            using var json = JsonDocument.Parse(request); sent.Add(action);
            if (action == "graphics.instance.update") state = state with { Instances = [state.Instances[0] with { Revision = 4 }] };
            else Assert.Equal(4, json.RootElement.GetProperty("expectedRevision").GetInt32());
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = "featured", CurrentRevision = 4 });
        }, () => state);
        workspace.Observe(state); workspace.Select("featured"); workspace.SetField("name", Value("New"));
        Assert.True(await workspace.CueAsync());
        Assert.Equal(new[] { "graphics.instance.update", "graphics.instance.cue" }, sent);
        workspace.Observe(state with { CoreEpoch = "core-two" });
        Assert.Null(workspace.Draft); Assert.Equal("", workspace.SelectedId);
        Assert.False(await workspace.ShowAsync()); Assert.Equal(2, sent.Count);
    }
    [Fact]
    public void TypedTemplateFieldsRoundTripWithoutInventingNameAndTitle()
    {
        var state = Initial with { Templates = [new() { TemplateId = "title", TemplateVersion = 2, Runtime = "declarative", Fields = [new() { Id = "heading", Type = "string" }] }],
            Instances = [new() { InstanceId = "title-one", TemplateId = "title", TemplateVersion = 2, Revision = 1,
                Fields = new() { AdditionalFields = new() { ["heading"] = Value("Welcome") } } }] };
        var workspace = new GraphicsWorkspaceSession((_, _, _) => throw new InvalidOperationException(), () => state);
        workspace.Observe(state); workspace.Select("title-one");
        Assert.Equal("Welcome", workspace.Draft!.Fields.GetProperty("heading").GetString());
        Assert.False(workspace.Draft.Fields.TryGetProperty("name", out _)); Assert.False(workspace.Draft.Fields.TryGetProperty("title", out _));
    }
    [Fact]
    public void RequiredAndUtf8BoundsValidatePrivatePreviewWithoutReplacingExplicitBlankSecondaryText()
    {
        var state = Initial with { Templates = [new() { TemplateId = "interview", TemplateVersion = 1, Runtime = "declarative",
            Fields = [new() { Id = "name", Type = "string", Required = true, MaxLength = 256 }, new() { Id = "title", Type = "string", MaxLength = 512 }] }],
            Instances = [new() { InstanceId = "one", TemplateId = "interview", TemplateVersion = 1, Revision = 1, Fields = new() { Name = "", Title = "" } }] };
        var sends = 0;
        var workspace = new GraphicsWorkspaceSession((_,_,_) => { sends++; return Task.FromResult(new NativeGraphicsOperationResult()); },() => state);
        workspace.Observe(state); workspace.Select("one"); Assert.Contains("required",workspace.PreviewValidation);
        workspace.SetField("name",Value("Ada")); Assert.Equal("",workspace.PreviewValidation);
        Assert.Equal("",workspace.Draft!.Fields.GetProperty("title").GetString());
        workspace.SetField("name",Value(new string('界',100))); Assert.Contains("type and range",workspace.PreviewValidation);
        Assert.Equal(0,sends);
    }
    [Fact]
    public async Task OutOfBandChangesCannotOverwritePrivateDraftAndCueUsesCorrelatedCommit()
    {
        var state = Initial;
        var sent = new List<JsonElement>();
        var workspace = new GraphicsWorkspaceSession((action, request, _) => {
            var body = JsonDocument.Parse(request).RootElement.Clone(); sent.Add(body);
            if (action == "graphics.instance.update") {
                Assert.Equal(3,body.GetProperty("expectedRevision").GetInt32());
                // Revision 5 arrived from another client after our revision 4.
                state = state with { Instances = [state.Instances[0] with { Revision = 5 }] };
                return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = "featured", CurrentRevision = 4 });
            }
            Assert.Equal(4,body.GetProperty("expectedRevision").GetInt32());
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = false, Reason = "revision-conflict" });
        }, () => state);
        workspace.Observe(state); workspace.Select("featured"); workspace.SetField("name",Value("My edit"));
        state = state with { Instances = [state.Instances[0] with { Revision = 9 }] }; workspace.Observe(state);
        Assert.False(await workspace.CueAsync()); Assert.Equal(2,sent.Count);
        Assert.Equal("My edit",workspace.Draft!.Fields.GetProperty("name").GetString());
        Assert.Equal("revision-conflict",workspace.Status);
    }

    [Fact]
    public async Task BrowsingIsPrivateAndCreationUsesEditedDraftWithPlacement()
    {
        var template=new NativeGraphicsTemplate { TemplateId="centered-title",TemplateVersion=1,Runtime="declarative",DefaultPlacement=new() { Anchor="center",X=0,Y=0 },Fields=[new() { Id="heading",Type="string",Default=Value("Welcome") }] };
        var state=Initial with { Templates=[template] }; JsonElement sent=default;
        var workspace=new GraphicsWorkspaceSession((_,request,_) => { sent=JsonDocument.Parse(request).RootElement.Clone(); return Task.FromResult(new NativeGraphicsOperationResult { Accepted=true }); },()=>state);
        workspace.Observe(state); workspace.Select("featured"); workspace.SetField("name",Value("Unsaved guest"));
        workspace.Browse(template); Assert.Null(workspace.Selected); Assert.Equal("center",workspace.Draft!.Placement!.Anchor);
        workspace.SetField("heading",Value("Tonight")); workspace.SetPlacement(workspace.Draft.Placement with { X=14 });
        Assert.True(await workspace.CreateAsync(template));
        Assert.Equal("Tonight",sent.GetProperty("fields").GetProperty("heading").GetString()); Assert.Equal(14,sent.GetProperty("placement").GetProperty("x").GetDouble());
        workspace.Select("featured"); Assert.Equal("Unsaved guest",workspace.Draft!.Fields.GetProperty("name").GetString());
        workspace.CancelDraft(); Assert.Equal("Private",workspace.Draft.Fields.GetProperty("name").GetString());
    }

    [Fact]
    public async Task ClockUsesSameApiBoundaryAndPrivateEditsCannotWriteGeneratedTime()
    {
        var state=Initial with { Instances=[Initial.Instances[0] with { TemplateId="clock-panel",Clock=new() { Text="12:00:00",State="wall" },Fields=new() { AdditionalFields=new() { ["label"]=Value("LIVE"),["time"]=Value("12:00:00") } } }] };
        var sent=new List<(string Action,JsonElement Body)>();
        var workspace=new GraphicsWorkspaceSession((action,request,_) => { sent.Add((action,JsonDocument.Parse(request).RootElement.Clone())); return Task.FromResult(new NativeGraphicsOperationResult { Accepted=true }); },()=>state);
        workspace.Observe(state); workspace.Select("featured");
        Assert.True(await workspace.ClockAsync("configure",new() { Mode="countdown",DurationSeconds=90 }));
        Assert.Equal("graphics.clock.control",sent[0].Action); Assert.Equal(90,sent[0].Body.GetProperty("configuration").GetProperty("durationSeconds").GetInt32());
        Assert.True(await workspace.UpdateProgramAsync()); Assert.False(sent[1].Body.GetProperty("fields").TryGetProperty("time",out _));
        Assert.DoesNotContain(sent,s=>s.Action=="graphics.instance.trigger");
    }

}
