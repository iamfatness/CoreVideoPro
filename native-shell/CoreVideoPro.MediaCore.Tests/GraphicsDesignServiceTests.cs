using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsDesignServiceTests : IDisposable
{
    private const string LibraryFull = "The graphics library is full. Delete designs you no longer use, or remove graphics from the playlist.";
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-graphics-designs-" + Guid.NewGuid().ToString("N"));
    private readonly List<(string Action, JsonElement Body)> _commands = [];
    private int _hex, _busyRemoves;
    private bool _coreDown, _cancelRemoves;
    private GraphicsWorkspaceSession? _session;
    private static readonly string[] BuiltIns = ["compact-solid", "minimal-accent", "broadcast", "centered-title", "corner-logo", "clock-panel"];

    private NativeGraphicsState _state = new() {
        CoreEpoch = "core-a", SchemaVersion = 1,
        Templates = BuiltIns.Select(id => Installed(JsonSerializer.SerializeToElement(new { templateId = id, templateVersion = 1, name = id, family = id == "centered-title" ? "title" : "lower-third" }))).ToArray()
    };

    private static NativeGraphicsTemplate Installed(JsonElement definition, string packageRoot = "") => new() {
        TemplateId = definition.GetProperty("templateId").GetString()!, TemplateVersion = definition.GetProperty("templateVersion").GetInt32(),
        Name = definition.TryGetProperty("name", out var name) ? name.GetString()! : "", Family = definition.TryGetProperty("family", out var family) ? family.GetString()! : "",
        Runtime = "declarative", ManifestHash = Hash(definition.GetRawText()), Definition = definition.Clone(), PackageRoot = packageRoot };
    private static string Hash(string text) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(text))).ToLowerInvariant();

    private Action? _onObserve;
    private NativeGraphicsState? Observe() { _onObserve?.Invoke(); return _coreDown ? null : _state; }
    private Task<NativeGraphicsOperationResult> Invoke(string action, string body, CancellationToken cancellation)
    {
        var input = JsonDocument.Parse(body).RootElement.Clone(); _commands.Add((action, input));
        var requestId = input.GetProperty("requestId").GetString()!;
        Assert.Equal(_state.CoreEpoch, input.GetProperty("coreEpoch").GetString());
        switch (action) {
            case "graphics.template.install": {
                var definition = JsonDocument.Parse(input.GetProperty("manifest").GetString()!).RootElement.Clone();
                if (_state.Templates.Count >= 16) {
                    _state = _state with { TemplateInstalls = [.. _state.TemplateInstalls, new() { RequestId = requestId, ClientId = input.GetProperty("clientId").GetString()!, State = "failed", Reason = "template-library-capacity" }] };
                } else {
                    _state = _state with { Templates = [.. _state.Templates, Installed(definition, input.GetProperty("packageRoot").GetString()!)],
                        TemplateInstalls = [.. _state.TemplateInstalls, new() { RequestId = requestId, State = "installed" }] };
                }
                return Done(requestId, true, "template-install-queued");
            }
            case "graphics.template.remove": {
                var id = input.GetProperty("templateId").GetString()!; var version = input.GetProperty("templateVersion").GetInt32();
                if (_cancelRemoves) throw new OperationCanceledException();
                if (_busyRemoves > 0) { _busyRemoves--; return Done(requestId, false, "template-library-busy"); }
                if (BuiltIns.Contains(id)) return Done(requestId, false, "template-built-in");
                if (_state.Instances.Any(i => i.TemplateId == id && i.TemplateVersion == version)) return Done(requestId, false, "template-in-use");
                if (!_state.Templates.Any(t => t.TemplateId == id && t.TemplateVersion == version)) return Done(requestId, false, "template-not-found");
                _state = _state with { Templates = _state.Templates.Where(t => !(t.TemplateId == id && t.TemplateVersion == version)).ToArray() };
                return Done(requestId, true, "template-removed");
            }
        }
        return Done(requestId, true, "accepted");
    }
    private static Task<NativeGraphicsOperationResult> Done(string requestId, bool accepted, string reason) =>
        Task.FromResult(new NativeGraphicsOperationResult { RequestId = requestId, Accepted = accepted, Reason = reason });

    private (GraphicsWorkspaceLibrary Library, GraphicsDesignService Service) Build()
    {
        _session = new GraphicsWorkspaceSession(Invoke, Observe); _session.Observe(_state);
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(Path.Combine(_directory, "show.json")), _session);
        return (library, new GraphicsDesignService(library, Invoke, Observe, (_, _) => Task.CompletedTask, () => DateTime.UtcNow, () => (++_hex).ToString("x6")));
    }
    private IEnumerable<JsonElement> Bodies(string action) => _commands.Where(c => c.Action == action).Select(c => c.Body);
    private bool IsInstalled(string id, int version) => _state.Templates.Any(t => t.TemplateId == id && t.TemplateVersion == version);
    private static NativeGraphicsScratch Edited(GraphicsDesign design, string heading)
    {
        var manifest = JsonNode.Parse(design.Definition.GetRawText())!.AsObject(); manifest["name"] = heading;
        return NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(manifest));
    }
    private async Task PinToPlaylist(GraphicsWorkspaceLibrary library, GraphicsDesign design, string rowName = "Guest name")
    {
        _session!.Observe(_state);
        var row = new GraphicsPlaylistRow { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = rowName, TemplateId = design.TemplateId, TemplateVersion = design.TemplateVersion };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
    }
    private void RestartCore() => _state = _state with { CoreEpoch = "core-b", Templates = _state.Templates.Where(t => BuiltIns.Contains(t.TemplateId)).ToArray(), TemplateInstalls = [], Instances = [] };

    [Fact] public async Task CreateBlankStoresADesignAndInstallsVersionOne()
    {
        var (library, service) = Build();
        var result = await service.CreateBlankAsync("lower-third", "Guest strap");
        Assert.True(result.Ok, result.Message);
        var design = Assert.Single(library.Document.Designs);
        Assert.StartsWith("design-guest-strap-", design.TemplateId);
        Assert.True(GraphicsDesignIds.IsDesignTemplateId(design.TemplateId));
        Assert.Equal(("Guest strap", "lower-third", 1), (design.Name, design.Family, design.TemplateVersion));
        Assert.Equal(design.TemplateId, design.Definition.GetProperty("templateId").GetString());
        Assert.True(IsInstalled(design.TemplateId, 1));
        Assert.Equal(design.TemplateId, result.Template?.TemplateId);
        var reloaded = await new GraphicsDocumentStore(Path.Combine(_directory, "show.json")).LoadAsync();
        Assert.Equal(design.DesignId, Assert.Single(reloaded.Document.Designs).DesignId);
    }

    [Fact] public async Task CreateFromCopiesABuiltInUnderANewIdAndLeavesTheBuiltInAlone()
    {
        var (library, service) = Build();
        var before = _state.Templates.Single(t => t.TemplateId == "centered-title");
        var result = await service.CreateFromAsync("centered-title", 1, "Show title");
        Assert.True(result.Ok, result.Message);
        var design = Assert.Single(library.Document.Designs);
        Assert.NotEqual("centered-title", design.TemplateId);
        Assert.True(GraphicsDesignIds.IsDesignTemplateId(design.TemplateId));
        Assert.Equal("title", design.Family);
        Assert.Equal("Show title", design.Definition.GetProperty("name").GetString());
        Assert.Same(before, _state.Templates.Single(t => t.TemplateId == "centered-title"));
        Assert.Empty(Bodies("graphics.template.remove"));
        Assert.True(IsInstalled(design.TemplateId, 1));
    }

    [Fact] public async Task SaveInstallsTheNextVersionStoresItAndRemovesTheUnusedOldVersion()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        var saved = await service.SaveAsync(design.DesignId, Edited(design, "Opening title v2"));
        Assert.True(saved.Ok, saved.Message);
        var stored = Assert.Single(library.Document.Designs);
        Assert.Equal(2, stored.TemplateVersion);
        Assert.Equal(2, stored.Definition.GetProperty("templateVersion").GetInt32());
        Assert.Equal("Opening title v2", stored.Definition.GetProperty("name").GetString());
        Assert.True(IsInstalled(design.TemplateId, 2));
        Assert.False(IsInstalled(design.TemplateId, 1));
        Assert.Equal(1, Assert.Single(Bodies("graphics.template.remove")).GetProperty("templateVersion").GetInt32());
    }

    [Fact] public async Task SaveKeepsTheOldVersionWhileAPlaylistRowPinsIt()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("lower-third", "Guest strap")).Design!;
        await PinToPlaylist(library, design);
        Assert.True((await service.SaveAsync(design.DesignId, Edited(design, "v2"))).Ok);
        Assert.True(IsInstalled(design.TemplateId, 1));
        Assert.True(IsInstalled(design.TemplateId, 2));
        Assert.Empty(Bodies("graphics.template.remove"));
        Assert.Equal(1, library.Document.Playlist.Single().TemplateVersion);
    }

    [Fact] public async Task SaveKeepsAVersionALiveInstanceOrSavedCueStillUses()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("bug", "Corner bug")).Design!;
        _state = _state with { Instances = [new() { InstanceId = "graphic-a", TemplateId = design.TemplateId, TemplateVersion = 1, Revision = 1 }] };
        Assert.True((await service.SaveAsync(design.DesignId, Edited(design, "v2"))).Ok);
        Assert.True(IsInstalled(design.TemplateId, 1));
        Assert.Empty(Bodies("graphics.template.remove"));
    }

    [Fact] public async Task SaveRetriesARemovalWhileTheLibraryIsBusy()
    {
        var (_, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        _busyRemoves = 2;
        Assert.True((await service.SaveAsync(design.DesignId, Edited(design, "v2"))).Ok);
        Assert.False(IsInstalled(design.TemplateId, 1));
        Assert.Equal(3, Bodies("graphics.template.remove").Count());
    }

    [Fact] public async Task DeleteIsRefusedWithTheRowNamesWhileAPlaylistRowPinsTheDesign()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("lower-third", "Guest strap")).Design!;
        await PinToPlaylist(library, design);
        var result = await service.DeleteAsync(design.DesignId);
        Assert.False(result.Ok);
        Assert.Contains("Used by: Guest name", result.Message);
        Assert.Single(library.Document.Designs);
        Assert.True(IsInstalled(design.TemplateId, 1));
        Assert.Empty(Bodies("graphics.template.remove"));
    }

    [Fact] public async Task DeleteRemovesEveryInstalledVersionAndTheDesign()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        _state = _state with { Instances = [new() { InstanceId = "graphic-a", TemplateId = design.TemplateId, TemplateVersion = 1, Revision = 1 }] };
        Assert.True((await service.SaveAsync(design.DesignId, Edited(design, "v2"))).Ok);
        _state = _state with { Instances = [] };
        var result = await service.DeleteAsync(design.DesignId);
        Assert.True(result.Ok, result.Message);
        Assert.Empty(library.Document.Designs);
        Assert.False(IsInstalled(design.TemplateId, 1));
        Assert.False(IsInstalled(design.TemplateId, 2));
    }

    [Fact] public async Task OpenInstallsTheCurrentVersionWhenTheCoreDoesNotHaveIt()
    {
        var (_, service) = Build();
        var design = (await service.CreateBlankAsync("full-screen", "Holding slate")).Design!;
        RestartCore();
        Assert.False(IsInstalled(design.TemplateId, 1));
        _commands.Clear();
        var opened = await service.OpenAsync(design.DesignId);
        Assert.True(opened.Ok, opened.Message);
        Assert.Equal((design.TemplateId, 1), (opened.Template!.TemplateId, opened.Template.TemplateVersion));
        Assert.True(IsInstalled(design.TemplateId, 1));
        _commands.Clear();
        Assert.True((await service.OpenAsync(design.DesignId)).Ok);
        Assert.Empty(Bodies("graphics.template.install"));
    }

    [Fact] public async Task ACapacityRefusalSurfacesTheLibraryFullMessage()
    {
        var (library, service) = Build();
        _state = _state with { Templates = [.. _state.Templates, .. Enumerable.Range(1, 10).Select(n => Installed(JsonSerializer.SerializeToElement(new { templateId = "look-" + n, templateVersion = 1 })))] };
        var result = await service.CreateBlankAsync("lower-third", "One too many");
        Assert.False(result.Ok);
        Assert.Equal(LibraryFull, result.Message);
        Assert.Empty(library.Document.Designs);
    }

    [Fact] public async Task EnsureInstalledForPlaylistInstallsOnlyPinnedDesigns()
    {
        var (library, service) = Build();
        var pinned = (await service.CreateBlankAsync("lower-third", "Guest strap")).Design!;
        var unused = (await service.CreateBlankAsync("title", "Unused title")).Design!;
        await PinToPlaylist(library, pinned);
        RestartCore(); _commands.Clear();
        await service.EnsureInstalledForPlaylistAsync();
        Assert.True(IsInstalled(pinned.TemplateId, 1));
        Assert.False(IsInstalled(unused.TemplateId, 1));
        Assert.Single(Bodies("graphics.template.install"));
    }

    [Fact] public async Task DuplicateMakesANewDesignWithTheSameDefinition()
    {
        var (library, service) = Build();
        var original = (await service.CreateBlankAsync("bug", "Corner bug")).Design!;
        var copy = await service.DuplicateAsync(original.DesignId, "Corner bug copy");
        Assert.True(copy.Ok, copy.Message);
        Assert.Equal(2, library.Document.Designs.Count);
        Assert.NotEqual(original.DesignId, copy.Design!.DesignId);
        Assert.NotEqual(original.TemplateId, copy.Design.TemplateId);
        Assert.Equal(copy.Design.TemplateId, copy.Design.Definition.GetProperty("templateId").GetString());
        Assert.Equal(original.Definition.GetProperty("layers").GetRawText(), copy.Design.Definition.GetProperty("layers").GetRawText());
    }

    [Fact] public async Task OtherLibrarySavesKeepTheDesigns()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        Assert.True(await library.SaveShotLayoutsAsync([]), library.Status);
        Assert.Equal(design.DesignId, Assert.Single(library.Document.Designs).DesignId);
    }

    [Fact] public void TemplateRemovePassesTheGateway()
    {
        var observed = new NativeGraphicsState { SchemaVersion = 1, CoreEpoch = "test-core" };
        static string Request(string extra) => "{\"schemaVersion\":1,\"requestId\":\"remove-one\",\"clientId\":\"operator\",\"coreEpoch\":\"test-core\"" + extra + "}";
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.template.remove", Request(",\"templateId\":\"design-strap-a1b2c3\",\"templateVersion\":2"), observed, out var command, out _, out var error), error);
        Assert.Equal("graphics-template-remove", command!.Type);
        Assert.Contains("graphics.template.remove", GraphicsCommandRequest.Actions);
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.template.remove", Request(",\"templateId\":\"design-strap-a1b2c3\""), observed, out _, out _, out _));
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.template.remove", Request(",\"templateId\":\"design-strap-a1b2c3\",\"templateVersion\":0"), observed, out _, out _, out _));
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.template.remove", Request(",\"templateVersion\":2"), observed, out _, out _, out _));
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.template.remove", Request(",\"templateId\":\"Bad Id\",\"templateVersion\":2"), observed, out _, out _, out _));
    }

    [Fact] public async Task ACoreRestartAfterTheNewVersionIsStoredStillReportsTheSaveAsDone()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        _onObserve = () => {
            if (library.Document.Designs.Any(d => d.TemplateVersion == 2)) { _state = _state with { CoreEpoch = "core-b" }; _onObserve = null; }
        };
        var saved = await service.SaveAsync(design.DesignId, Edited(design, "v2"));
        Assert.True(saved.Ok, saved.Message);
        Assert.Contains("restarted", saved.Message);
        Assert.Equal(2, Assert.Single(library.Document.Designs).TemplateVersion);
        Assert.Equal(2, saved.Design!.TemplateVersion);
    }

    [Fact] public async Task OpenReinstallsWithThePinnedPackageRoot()
    {
        var (library, service) = Build();
        _state = _state with { Templates = [.. _state.Templates, Installed(JsonSerializer.SerializeToElement(new { templateId = "package-look", templateVersion = 1, name = "Package look", family = "lower-third" }), @"D:\looks\package")] };
        var design = (await service.CreateFromAsync("package-look", 1, "Package strap")).Design!;
        await PinToPlaylist(library, design);
        Assert.Equal(@"D:\looks\package", library.Document.Templates.Single(t => t.TemplateId == design.TemplateId).PackageRoot);
        RestartCore(); _commands.Clear();
        Assert.True((await service.OpenAsync(design.DesignId)).Ok);
        Assert.Equal(@"D:\looks\package", Assert.Single(Bodies("graphics.template.install")).GetProperty("packageRoot").GetString());
    }

    [Fact] public async Task SaveRefusesAMalformedDraftInsteadOfThrowing()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        var result = await service.SaveAsync(design.DesignId, new NativeGraphicsScratch("{not json", "x"));
        Assert.False(result.Ok);
        Assert.Equal("The design is not a valid template.", result.Message);
        Assert.Equal(1, Assert.Single(library.Document.Designs).TemplateVersion);
    }

    // ---- GraphicsDesignWorkflow (operator steps over the service and the private draft) ----
    private (GraphicsWorkspaceLibrary Library, GraphicsDesignService Service, GraphicsDesignWorkflow Workflow) BuildWorkflow()
    {
        var (library, service) = Build();
        return (library, service, new GraphicsDesignWorkflow(service, _session!, library, Observe));
    }
    private NativeGraphicsTemplate InstallLook(string id, string kind)
    {
        var look = Installed(JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest(kind, id, 1, "Look " + kind)));
        _state = _state with { Templates = [.. _state.Templates, look] }; _session!.Observe(_state);
        return look;
    }
    private static NativeGraphicsTemplate Template(string id, int version, string family) => new() { TemplateId = id, TemplateVersion = version, Family = family, Name = id, Runtime = "declarative" };
    private static NativeGraphicsDraft Draft(string id, int version) => new(id, version, JsonSerializer.SerializeToElement(new { name = "Ada", title = "Host" }), LowerThirdAppearance.FromPreset(id), new());
    private static GraphicsDesign DesignOf(string id, string family) => new(id, "Guest strap", family, id, 2, JsonSerializer.SerializeToElement(new { templateId = id }), [], DateTimeOffset.UtcNow);

    [Fact] public void PlaylistRowForANewLowerThirdTakesTheNextFreeKeyFollowsSourcesAndPlaysOutAfterEightSeconds()
    {
        var rows = new[] { new GraphicsPlaylistRow { Id = "logo", Kind = GraphicsPlaylistRow.Logo, Name = "Logo", Hotkey = "F1", TemplateId = "corner-logo", TemplateVersion = 1 } };
        var row = GraphicsDesignWorkflow.PlaylistRow(DesignOf("design-strap-a1b2c3", "lower-third"), Template("design-strap-a1b2c3", 2, "lower-third"), Draft("design-strap-a1b2c3", 2), rows);
        Assert.Equal(("F2", GraphicsPlaylistRow.Guest, 8000, true, 2), (row.Hotkey, row.Kind, row.PlayOutAfterMs, row.FollowSources, row.TemplateVersion));
        Assert.Equal("Guest strap", row.Name);
        var fullScreen = GraphicsDesignWorkflow.PlaylistRow(DesignOf("design-slate-a1b2c3", "title"), Template("design-slate-a1b2c3", 2, "title"), Draft("design-slate-a1b2c3", 2), rows);
        Assert.Equal((GraphicsPlaylistRow.Title, 0), (fullScreen.Kind, fullScreen.PlayOutAfterMs));
    }

    [Fact] public void PlaylistRowRepinsTheDesignsExistingRowAndKeepsItsKeyNameAndTiming()
    {
        var existing = new GraphicsPlaylistRow { Id = "strap", Kind = GraphicsPlaylistRow.Title, Name = "Opening strap", Hotkey = "F6", PlayOutAfterMs = 15000, TemplateId = "design-strap-a1b2c3", TemplateVersion = 1 };
        var rows = new[] { new GraphicsPlaylistRow { Id = "logo", Kind = GraphicsPlaylistRow.Logo, Name = "Logo", Hotkey = "F1", TemplateId = "corner-logo", TemplateVersion = 1 }, existing };
        var row = GraphicsDesignWorkflow.PlaylistRow(DesignOf("design-strap-a1b2c3", "lower-third"), Template("design-strap-a1b2c3", 2, "lower-third"), Draft("design-strap-a1b2c3", 2), rows);
        Assert.Equal(("strap", "Opening strap", "F6", 15000, false, 2), (row.Id, row.Name, row.Hotkey, row.PlayOutAfterMs, row.FollowSources, row.TemplateVersion));
    }

    [Fact] public async Task OpenMakesTheDesignThePrivateDraftAndSaysSo()
    {
        var (_, service, workflow) = BuildWorkflow();
        var design = (await service.CreateBlankAsync("lower-third", "Guest strap")).Design!;
        var opened = 0; workflow.Opened += (_, _) => opened++;
        var result = await workflow.OpenAsync(design.DesignId);
        Assert.True(result.Ok, result.Message);
        Assert.Equal((design.TemplateId, 1), (_session!.Draft!.TemplateId, _session.Draft.TemplateVersion));
        Assert.Equal(1, opened);
        Assert.Same(design.DesignId, workflow.DesignFor(_session.Draft)!.DesignId);
    }

    [Fact] public async Task SaveOnALookMakesOneCopyAndARetryAfterAFailedSaveGoesIntoThatCopy()
    {
        var (library, _, workflow) = BuildWorkflow();
        var look = InstallLook("look-title", "title");
        _state = _state with { Templates = [.. _state.Templates, .. Enumerable.Range(1, 8).Select(n => Installed(JsonSerializer.SerializeToElement(new { templateId = "filler-" + n, templateVersion = 1 })))] };
        _session!.Observe(_state); _session.Browse(look);
        _session.EditTemplate(d => GraphicsTemplateEditor.AddField(d, "location", "text", 40));
        var failed = await workflow.SaveAsync(); // the copy fills slot 16, so its first save is refused
        Assert.False(failed.Ok);
        Assert.StartsWith("Created Look title copy; your edits were not saved: " + LibraryFull, failed.Message);
        var design = Assert.Single(library.Document.Designs);
        Assert.Equal(design.TemplateId, _session.Draft!.TemplateId);
        Assert.Contains("\"location\"", _session.Draft.Scratch!.Manifest);
        _state = _state with { Templates = _state.Templates.Where(t => t.TemplateId != "filler-1").ToArray() }; _session.Observe(_state);
        var saved = await workflow.SaveAsync();
        Assert.True(saved.Ok, saved.Message);
        Assert.Equal(2, Assert.Single(library.Document.Designs).TemplateVersion);
        Assert.Equal((design.TemplateId, 2), (_session.Draft!.TemplateId, _session.Draft.TemplateVersion));
        Assert.Contains(library.Document.Designs[0].Definition.GetProperty("fields").EnumerateArray(), f => f.GetProperty("id").GetString() == "location");
    }

    [Fact] public async Task SaveToPlaylistSavesThenPinsOneRowOnTheNextFreeKey()
    {
        var (library, _, workflow) = BuildWorkflow();
        Assert.True((await workflow.CreateAsync("lower-third", null, 0, "Guest strap")).Ok);
        _session!.EditTemplate(d => GraphicsTemplateEditor.AddField(d, "location", "text", 40));
        var result = await workflow.SaveToPlaylistAsync();
        Assert.True(result.Ok, result.Message);
        Assert.Contains("is in the playlist on F1", result.Message);
        var row = Assert.Single(library.Document.Playlist);
        Assert.Equal((result.Design!.TemplateId, 2, "F1"), (row.TemplateId, row.TemplateVersion, row.Hotkey));
        Assert.True((await workflow.SaveToPlaylistAsync()).Ok);
        Assert.Single(library.Document.Playlist);
    }

    [Fact] public async Task CreateReportsAStoredDesignAsCreatedEvenWhenTheEditorIsBusy()
    {
        var (library, service) = Build();
        var gate = new TaskCompletionSource<NativeGraphicsOperationResult>();
        var busy = new GraphicsWorkspaceSession((_, _, _) => gate.Task, Observe); busy.Observe(_state);
        var workflow = new GraphicsDesignWorkflow(service, busy, library, Observe);
        var pending = busy.RefreshCatalogAsync();
        Assert.True(busy.Busy);
        var result = await workflow.CreateAsync("title", null, 0, "Opening title");
        Assert.True(result.Ok, result.Message);
        Assert.EndsWith("It is stored; open it from Your designs.", result.Message);
        Assert.Single(library.Document.Designs);
        gate.SetResult(new NativeGraphicsOperationResult { Accepted = true }); await pending;
    }

    [Fact] public void ResyncFiresOncePerArmedEpoch()
    {
        var resync = new GraphicsDesignResync();
        NativeGraphicsState Epoch(string e) => new() { CoreEpoch = e, SchemaVersion = 1 };
        Assert.False(resync.Observe(Epoch("a")));      // not armed
        Assert.False(resync.Arm(null));                // armed, no graphics state yet
        Assert.True(resync.Observe(Epoch("a")));
        Assert.False(resync.Observe(Epoch("a")));
        Assert.False(resync.Arm(Epoch("a")));          // re-armed while the old epoch is still reported
        Assert.True(resync.Observe(Epoch("b")));
        Assert.False(resync.Observe(Epoch("c")));      // a new epoch without a new generation does not fire
    }

    // ---- final review fixes ------------------------------------------------------------------
    private int DesignSlots => _state.Templates.Count(t => GraphicsDesignIds.IsDesignTemplateId(t.TemplateId));

    [Fact] public async Task SaveToPlaylistTwentyTimesKeepsOnlyTheReferencedPins()
    {
        var (library, _, workflow) = BuildWorkflow();
        Assert.True((await workflow.CreateAsync("lower-third", null, 0, "Guest strap")).Ok);
        for (var i = 0; i < 20; i++) {
            var field = "extra-" + i;
            _session!.EditTemplate(d => GraphicsTemplateEditor.AddField(d, field, "text", 40));
            var result = await workflow.SaveToPlaylistAsync();
            Assert.True(result.Ok, $"save {i + 1}: {result.Message}");
        }
        var row = Assert.Single(library.Document.Playlist);
        Assert.Equal(21, row.TemplateVersion);
        var pin = Assert.Single(library.Document.Templates);
        Assert.Equal((row.TemplateId, row.TemplateVersion), (pin.TemplateId, pin.TemplateVersion));
        var reloaded = await new GraphicsDocumentStore(Path.Combine(_directory, "show.json")).LoadAsync();
        Assert.Single(reloaded.Document.Templates);
        Assert.Equal(1, DesignSlots);
    }

    [Fact] public async Task DeletingADesignRemovesItsPins()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("lower-third", "Guest strap")).Design!;
        var other = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        await PinToPlaylist(library, design);
        var titleRow = new GraphicsPlaylistRow { Id = "title", Kind = GraphicsPlaylistRow.Title, Name = "Show title", TemplateId = other.TemplateId, TemplateVersion = 1 };
        Assert.True(await library.SavePlaylistAsync([titleRow]), library.Status);
        Assert.True((await service.DeleteAsync(design.DesignId)).Ok);
        Assert.DoesNotContain(library.Document.Templates, t => t.TemplateId == design.TemplateId);
        Assert.Contains(library.Document.Templates, t => t.TemplateId == other.TemplateId);
    }

    [Fact] public async Task SaveToPlaylistKeepsTheValuesTypedIntoTheDraft()
    {
        var (library, _, workflow) = BuildWorkflow();
        Assert.True((await workflow.CreateAsync("lower-third", null, 0, "Guest strap")).Ok);
        _session!.EditTemplate(d => GraphicsTemplateEditor.AddField(d, "location", "text", 40));
        _session.SetField("location", JsonSerializer.SerializeToElement("London"));
        _session.SetStyle(_session.Draft!.Style with { AccentColor = "#FF8800" });
        var result = await workflow.SaveToPlaylistAsync();
        Assert.True(result.Ok, result.Message);
        var row = Assert.Single(library.Document.Playlist);
        Assert.Equal("London", row.Fields["location"].GetString());
        Assert.Equal("#FF8800", row.Style.AccentColor);
        Assert.Equal("London", _session.Draft!.Fields.GetProperty("location").GetString());
        Assert.Equal(2, _session.Draft.TemplateVersion);
    }

    [Fact] public async Task OpeningTwelveDesignsInSequenceNeverFillsTheLibrary()
    {
        var (library, service, workflow) = BuildWorkflow();
        var ids = new List<string>();
        for (var i = 0; i < 12; i++) {
            var created = await service.CreateBlankAsync("bug", "Bug " + i);
            Assert.True(created.Ok, $"create {i}: {created.Message}");
            ids.Add(created.Design!.DesignId);
        }
        RestartCore(); _session!.Observe(_state);
        foreach (var id in ids) {
            var opened = await workflow.OpenAsync(id);
            Assert.True(opened.Ok, opened.Message);
            Assert.Equal(library.Document.Designs.Single(d => d.DesignId == id).TemplateId, _session.Draft!.TemplateId);
        }
        Assert.Equal(1, DesignSlots);
    }

    [Fact] public async Task ALibraryFullInstallEvictsUnreferencedDesignsButNeverAReferencedOrOpenOne()
    {
        var (library, service, workflow) = BuildWorkflow();
        var pinned = (await service.CreateBlankAsync("lower-third", "Pinned strap")).Design!;
        await PinToPlaylist(library, pinned);
        var open = (await service.CreateBlankAsync("title", "Open title")).Design!;
        Assert.True((await workflow.OpenAsync(open.DesignId)).Ok);
        for (var i = 0; i < 12; i++) {
            var created = await service.CreateBlankAsync("bug", "Bug " + i);
            Assert.True(created.Ok, $"create {i}: {created.Message}");
            Assert.True(IsInstalled(pinned.TemplateId, 1));
            Assert.True(IsInstalled(open.TemplateId, 1));
        }
        Assert.Equal(14, library.Document.Designs.Count);
    }

    [Fact] public async Task ALibraryFullOfReferencedVersionsStillRefusesWithAnActionableMessage()
    {
        var (library, service) = Build();
        var rows = new List<GraphicsPlaylistRow>();
        for (var i = 0; i < 10; i++) {
            var design = (await service.CreateBlankAsync("bug", "Bug " + i)).Design!;
            rows.Add(new() { Id = "bug-" + i, Kind = GraphicsPlaylistRow.Title, Name = "Bug " + i, TemplateId = design.TemplateId, TemplateVersion = 1 });
        }
        _session!.Observe(_state);
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        var refused = await service.CreateBlankAsync("bug", "One too many");
        Assert.False(refused.Ok);
        Assert.Equal(LibraryFull, refused.Message);
        Assert.Empty(Bodies("graphics.template.remove"));
    }

    [Fact] public async Task UnsavedEditsOnADesignTheAuthorMovedOffSurviveAFullLibrary()
    {
        var (_, service, workflow) = BuildWorkflow();
        var edited = (await service.CreateBlankAsync("title", "Edited title")).Design!;
        var other = (await service.CreateBlankAsync("bug", "Other bug")).Design!;
        Assert.True((await workflow.OpenAsync(edited.DesignId)).Ok);
        _session!.EditTemplate(d => GraphicsTemplateEditor.AddField(d, "location", "text", 40));
        Assert.True((await workflow.OpenAsync(other.DesignId)).Ok);
        Assert.True(IsInstalled(edited.TemplateId, 1));
        for (var i = 0; i < 12; i++) {
            var created = await service.CreateBlankAsync("bug", "Bug " + i);
            Assert.True(created.Ok, $"create {i}: {created.Message}");
        }
        Assert.True(IsInstalled(edited.TemplateId, 1));
        Assert.True((await workflow.OpenAsync(edited.DesignId)).Ok);
        Assert.Contains("\"location\"", _session.Draft!.Scratch!.Manifest);
    }

    [Fact] public async Task ACoreRestartDuringDeleteCleanupStillReportsTheDelete()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        _onObserve = () => {
            if (library.Document.Designs.Count == 0) { _state = _state with { CoreEpoch = "core-b" }; _onObserve = null; }
        };
        var deleted = await service.DeleteAsync(design.DesignId);
        Assert.True(deleted.Ok, deleted.Message);
        Assert.Contains("restarted", deleted.Message);
        Assert.Empty(library.Document.Designs);
    }

    [Fact] public async Task ACancelDuringCleanupDoesNotReportTheSaveOrDeleteAsCancelled()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        _cancelRemoves = true;
        var saved = await service.SaveAsync(design.DesignId, Edited(design, "v2"));
        Assert.True(saved.Ok, saved.Message);
        Assert.DoesNotContain("cancelled", saved.Message);
        Assert.Equal(2, Assert.Single(library.Document.Designs).TemplateVersion);
        var deleted = await service.DeleteAsync(design.DesignId);
        Assert.True(deleted.Ok, deleted.Message);
        Assert.DoesNotContain("cancelled", deleted.Message);
        Assert.Empty(library.Document.Designs);
    }

    [Fact] public async Task DuplicateAndDeleteWorkWhileTheCoreIsDown()
    {
        var (library, service) = Build();
        var design = (await service.CreateBlankAsync("bug", "Corner bug")).Design!;
        _coreDown = true; _commands.Clear();
        var copy = await service.DuplicateAsync(design.DesignId, "Corner bug copy");
        Assert.True(copy.Ok, copy.Message);
        Assert.Equal(2, library.Document.Designs.Count);
        var deleted = await service.DeleteAsync(design.DesignId);
        Assert.True(deleted.Ok, deleted.Message);
        Assert.Equal(copy.Design!.DesignId, Assert.Single(library.Document.Designs).DesignId);
        Assert.Empty(_commands);
        Assert.False((await service.CreateBlankAsync("bug", "Needs a core")).Ok);
    }

    [Fact] public async Task DeletingTheDesignOpenInTheEditorIsRefused()
    {
        var (library, service, workflow) = BuildWorkflow();
        var design = (await service.CreateBlankAsync("title", "Opening title")).Design!;
        Assert.True((await workflow.OpenAsync(design.DesignId)).Ok);
        var refused = await workflow.DeleteAsync(design.DesignId);
        Assert.False(refused.Ok);
        Assert.Contains("open in the editor", refused.Message);
        Assert.Single(library.Document.Designs);
        Assert.True(IsInstalled(design.TemplateId, 1));
        var other = (await service.CreateBlankAsync("bug", "Corner bug")).Design!;
        Assert.True((await workflow.OpenAsync(other.DesignId)).Ok);
        Assert.True((await workflow.DeleteAsync(design.DesignId)).Ok);
    }

    public void Dispose() { try { Directory.Delete(_directory, true); } catch (IOException) { } }
}
