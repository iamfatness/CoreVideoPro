using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class StudioLowerThirdAssignmentTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-studio-look-" + Guid.NewGuid().ToString("N"));
    private readonly List<(string Action, JsonElement Body)> _commands = [];
    private NativeGraphicsState _state = new() {
        CoreEpoch = "core-a", SchemaVersion = 1,
        Templates = [new() { TemplateId = "compact-solid", TemplateVersion = 1, Name = "Compact solid", Family = "lower-third", Runtime = "compatibility" }],
        Instances = [new() { InstanceId = "guest", Label = "Guest look", Revision = 1, TemplateId = "compact-solid", TemplateVersion = 1, Scope = "global", Fields = new() { Name = "Original", Title = "" }, Style = new() { NameSize = 64 } }]
    };
    private GraphicsWorkspaceSession Session() { var session = new GraphicsWorkspaceSession(Invoke, () => _state); session.Observe(_state); session.Select("guest"); return session; }
    private GraphicsWorkspaceLibrary Library(GraphicsWorkspaceSession session) => new(new GraphicsDocumentStore(Path.Combine(_directory, "show.json")), session);
    private static SourceInstanceIdentity Identity(long generation) => new() { SourceId = "zoom:7", InstanceId = "camera-7", ProcessEpoch = "zoom-a", Generation = generation };
    private Task<NativeGraphicsOperationResult> Invoke(string action, string body, CancellationToken cancellation)
    {
        var input = JsonDocument.Parse(body).RootElement.Clone(); _commands.Add((action, input));
        var id = input.TryGetProperty("instanceId", out var rawId) ? rawId.GetString()! : "";
        if (action == "graphics.instance.create") {
            var item = new NativeGraphicsInstance { InstanceId = id, Revision = 1, TemplateId = "compact-solid", TemplateVersion = 1,
                Scope = "global", Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(input.GetProperty("fields"), new JsonSerializerOptions(JsonSerializerDefaults.Web))! };
            _state = _state with { Instances = [.. _state.Instances, item] };
        } else if (action == "graphics.instance.cue") {
            _state = _state with { Instances = _state.Instances.Select(i => i.InstanceId == id ? i with { CueRevision = i.Revision } : i).ToArray() };
        } else if (action == "graphics.instance.trigger") {
            _state = _state with { Instances = _state.Instances.Select(i => i.InstanceId == id ? i with { ProgramRevision = i.CueRevision, Visibility = "visible" } : i).ToArray() };
        } else if (action == "graphics.instance.remove") _state = _state with { Instances = _state.Instances.Where(i => i.InstanceId != id).ToArray() };
        return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = id, CurrentRevision = 1, Reason = "accepted" });
    }

    [Theory]
    [InlineData("capture:camera")]
    [InlineData("capture:screen:2")]
    public async Task LocalProgramSourceUsesExactNativeIdentityAndOperatorText(string sourceId)
    {
        var identity=new SourceInstanceIdentity {SourceId=sourceId,InstanceId=sourceId+"-generation-9",ProcessEpoch="core-a:capture",Generation=9};
        _state=_state with {Sources=[new() {VideoOn=true,Identity=identity,DisplayName=sourceId}]};
        var session=Session();var library=Library(session);
        Assert.True(await library.AssignStudioLowerThirdAsync(true));
        var controller=new StudioLowerThirdController(library,Invoke,()=>_state);
        Assert.True((await controller.ShowAsync("scene-a",_=>new(identity,"Operator camera name",""))).Accepted);
        var request=_commands.Single(c=>c.Action=="graphics.instance.create").Body;
        Assert.Equal(sourceId,request.GetProperty("binding").GetProperty("sourceId").GetString());
        Assert.Equal(9,request.GetProperty("binding").GetProperty("generation").GetInt32());
        Assert.Equal("Operator camera name",request.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("",request.GetProperty("fields").GetProperty("title").GetString());
    }

    [Fact] public async Task AssignmentPinsPrivateDraftAndNeverCuesOrShows()
    {
        var session = Session(); var library = Library(session);
        session.SetField("name", JsonSerializer.SerializeToElement("Pinned name"));
        Assert.True(await library.AssignStudioLowerThirdAsync(false));
        session.SetField("name", JsonSerializer.SerializeToElement("Later private edit"));
        Assert.Equal("Pinned name", library.Document.StudioLowerThird!.Cue.Draft!.Fields.GetProperty("name").GetString());
        Assert.Equal(64, library.Document.StudioLowerThird.Cue.Draft.Style.NameSize);
        Assert.Empty(_commands);
        var loaded = await new GraphicsDocumentStore(Path.Combine(_directory, "show.json")).LoadAsync();
        Assert.Equal("Pinned name", loaded.Document.StudioLowerThird!.Cue.Draft!.Fields.GetProperty("name").GetString());
        Assert.Empty(_commands);
    }

    [Fact] public async Task BrowsingDoesNotChangeThePinnedShowAndDoesNotStealEditorSelection()
    {
        var session = Session(); var library = Library(session);
        Assert.True(await library.AssignStudioLowerThirdAsync(false));
        session.Browse(_state.Templates[0]); session.SetField("name", JsonSerializer.SerializeToElement("Browse only"));
        var controller = new StudioLowerThirdController(library, Invoke, () => _state);
        Assert.True((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        Assert.Equal("Original", _commands.Single(c => c.Action == "graphics.instance.create").Body.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("", session.SelectedId);
        Assert.Equal("Browse only", session.Draft!.Fields.GetProperty("name").GetString());
        Assert.Equal(new[] { "graphics.instance.create", "graphics.instance.cue", "graphics.instance.trigger" }, _commands.Select(c => c.Action));
    }

    [Fact] public async Task ReassignmentDoesNotMutateAnOnAirInstance()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(false);
        var controller = new StudioLowerThirdController(library, Invoke, () => _state);
        Assert.True((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        var live = _state.Instances.Single(i => i.ProgramRevision > 0); var count = _commands.Count;
        session.SetField("name", JsonSerializer.SerializeToElement("Next show"));
        Assert.True(await library.AssignStudioLowerThirdAsync(false));
        Assert.Equal(count, _commands.Count);
        Assert.Equal(live, _state.Instances.Single(i => i.ProgramRevision > 0));
        Assert.False((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        Assert.Equal(count, _commands.Count);
    }

    [Fact] public async Task NextShowAfterHideRetiresOnlyItsHiddenCopyAndUsesReassignment()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(false);
        var controller = new StudioLowerThirdController(library, Invoke, () => _state);
        Assert.True((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        var first = _state.Instances.Single(i => i.ProgramRevision > 0).InstanceId;
        _state = _state with { Instances = _state.Instances.Select(i => i.InstanceId == first ? i with { ProgramRevision = 0, Visibility = "hidden" } : i).ToArray() };
        session.SetField("name", JsonSerializer.SerializeToElement("Next guest")); await library.AssignStudioLowerThirdAsync(false);
        Assert.True((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        Assert.Equal(first, _commands.Single(c => c.Action == "graphics.instance.remove").Body.GetProperty("instanceId").GetString());
        Assert.Contains(_state.Instances, i => i.InstanceId == "guest");
        Assert.Equal("Next guest", _state.Instances.Single(i => i.ProgramRevision > 0).Fields.Name);
    }

    [Fact] public async Task ChangedRevisionAndRestartCannotReplayShow()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(false);
        async Task<NativeGraphicsOperationResult> Changed(string action, string body, CancellationToken cancellation) {
            var result = await Invoke(action, body, cancellation);
            if (action == "graphics.instance.cue") _state = _state with { Instances = _state.Instances.Select(i => i.InstanceId == result.InstanceId ? i with { Revision = 2 } : i).ToArray() };
            return result;
        }
        var controller = new StudioLowerThirdController(library, Changed, () => _state);
        Assert.False((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        Assert.DoesNotContain(_commands, c => c.Action == "graphics.instance.trigger");
        _commands.Clear();
        async Task<NativeGraphicsOperationResult> Restart(string action, string body, CancellationToken cancellation) {
            var result = await Invoke(action, body, cancellation); _state = _state with { CoreEpoch = "replacement" }; return result;
        }
        controller = new StudioLowerThirdController(library, Restart, () => _state);
        Assert.False((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        Assert.Single(_commands); Assert.False(controller.Busy);
    }

    [Fact] public async Task ExternalFileChangePreservesPreviousAssignment()
    {
        var session=Session(); var library=Library(session); await library.AssignStudioLowerThirdAsync(false);
        var original=library.Document.StudioLowerThird;
        await File.AppendAllTextAsync(Path.Combine(_directory,"show.json"), " ");
        session.SetField("name",JsonSerializer.SerializeToElement("Rejected edit"));
        Assert.False(await library.AssignStudioLowerThirdAsync(false));
        Assert.Equal(original,library.Document.StudioLowerThird); Assert.Empty(_commands);
    }

    [Fact] public async Task SourceGenerationsAreNotPersistedAndProgramCopyUsesTheFreshIdentity()
    {
        _state = _state with { Sources = [new() { VideoOn = true, Identity = Identity(1), DisplayName = "Previous" }] };
        var session = Session(); session.SetBinding(Identity(1)); var library = Library(session);
        Assert.True(await library.AssignStudioLowerThirdAsync(true));
        Assert.Null(library.Document.StudioLowerThird!.Cue.Draft!.Binding);
        Assert.Equal("zoom:7", library.Document.StudioLowerThird.Cue.SourceId);
        _state = _state with { Sources = [new() { VideoOn = true, Identity = Identity(2), DisplayName = "Current" }] };
        var controller = new StudioLowerThirdController(library, Invoke, () => _state);
        Assert.True((await controller.ShowAsync("scene-a", _ => new(Identity(2), "Current guest", ""))).Accepted);
        var sent = _commands.Single(c => c.Action == "graphics.instance.create").Body;
        Assert.Equal(2, sent.GetProperty("binding").GetProperty("generation").GetInt32());
        Assert.Equal("Current guest", sent.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("", sent.GetProperty("fields").GetProperty("title").GetString());
    }

    [Fact] public async Task StaleSubjectAndWrongSceneRefuseWithoutNativeChanges()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(true);
        var controller = new StudioLowerThirdController(library, Invoke, () => _state);
        Assert.False((await controller.ShowAsync("scene-a", _ => new(Identity(1), "Stale", ""))).Accepted);
        Assert.Empty(_commands);
        _state = _state with { Instances = [_state.Instances[0] with { Scope = "scene", SceneId = "other" }] }; session.Observe(_state); session.Select("guest");
        await library.AssignStudioLowerThirdAsync(false);
        Assert.False((await controller.ShowAsync("scene-a", _ => null)).Accepted);
        Assert.Empty(_commands);
    }

    [Fact] public async Task CapacityRefusalNeverEvictsUserCuesOrShowsAnotherRevision()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(false);
        var controller = new StudioLowerThirdController(library, (_, _, _) => Task.FromResult(new NativeGraphicsOperationResult { Reason = "instance-capacity" }), () => _state);
        var before = _state.Instances;
        Assert.Equal("instance-capacity", (await controller.ShowAsync("scene-a", _ => null)).Reason);
        Assert.Equal(before, _state.Instances); Assert.Empty(_commands);
    }

    [Fact] public async Task PreviewSamplesAndNonLowerThirdsCannotOverwriteAnAssignment()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(false);
        var original = library.Document.StudioLowerThird;
        session.SetPreviewSample("long"); Assert.False(await library.AssignStudioLowerThirdAsync(false));
        session.SetPreviewSample("current"); _state = _state with { Templates = [_state.Templates[0] with { Family = "title" }] }; session.Observe(_state);
        Assert.False(await library.AssignStudioLowerThirdAsync(false));
        Assert.Equal(original, library.Document.StudioLowerThird); Assert.Empty(_commands);
    }

    [Fact] public async Task RemovingAssignmentLeavesLiveOutputAlone()
    {
        var session = Session(); var library = Library(session); await library.AssignStudioLowerThirdAsync(false);
        Assert.True(await library.ClearStudioLowerThirdAsync());
        Assert.Null(library.Document.StudioLowerThird); Assert.Empty(_commands);
    }
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
