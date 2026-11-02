using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsDesignServiceTests : IDisposable
{
    private const string LibraryFull = "The graphics library is full. Close another design or remove looks you no longer use.";
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-graphics-designs-" + Guid.NewGuid().ToString("N"));
    private readonly List<(string Action, JsonElement Body)> _commands = [];
    private int _hex, _busyRemoves;
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
    private NativeGraphicsState? Observe() { _onObserve?.Invoke(); return _state; }
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

    public void Dispose() { try { Directory.Delete(_directory, true); } catch (IOException) { } }
}
