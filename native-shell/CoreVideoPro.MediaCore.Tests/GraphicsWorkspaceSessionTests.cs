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
            return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true });
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
}
