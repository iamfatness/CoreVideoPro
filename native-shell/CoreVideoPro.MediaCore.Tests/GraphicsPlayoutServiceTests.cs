using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsPlayoutServiceTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-graphics-playout-" + Guid.NewGuid().ToString("N"));
    private readonly List<(string Action, JsonElement Body)> _commands = [];
    private readonly List<TimeSpan> _delays = [];
    private readonly Dictionary<TimeSpan, TaskCompletionSource> _heldDelays = [];
    private int _cuePendingPolls;
    private bool _showDoesNotLand;
    private string? _holdCueInstance;
    private string? _lostCueReply;
    private readonly TaskCompletionSource _heldCueReached = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private static NativeGraphicsTemplate Template(string id, string family, string runtime, params string[] fields) => new() {
        TemplateId = id, TemplateVersion = 1, Name = id, Family = family, Runtime = runtime, ManifestHash = Hash(id), Definition = JsonSerializer.SerializeToElement(new { templateId = id, templateVersion = 1 }),
        Fields = fields.Select(f => new NativeGraphicsTemplateField { Id = f, Type = "string", Default = JsonSerializer.SerializeToElement("") }).ToArray() };
    private static string Hash(string id) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(id))).ToLowerInvariant();
    private NativeGraphicsState _state = new() {
        CoreEpoch = "core-a", SchemaVersion = 1,
        Templates = [Template("compact-solid", "lower-third", "compatibility"), Template("minimal-accent", "lower-third", "compatibility"), Template("broadcast", "lower-third", "compatibility"),
            Template("centered-title", "title", "declarative", "heading", "subtitle"), Template("corner-logo", "bug", "declarative"),
            Template("clock-panel", "clock", "declarative", "label", "time")],
        Sources = [new() { VideoOn = true, DisplayName = "Ada", Identity = Identity("zoom:7") }, new() { VideoOn = true, DisplayName = "HD60", Identity = Identity("capture:camera") }]
    };
    private static SourceInstanceIdentity Identity(string sourceId) => new() { SourceId = sourceId, InstanceId = sourceId + "-gen-1", ProcessEpoch = "zoom-a", Generation = 1 };
    private NativeGraphicsState? Observe()
    {
        if (_cuePendingPolls > 0 && _state.Instances.Any(i => i.CuePending) && --_cuePendingPolls == 0)
            _state = _state with { Instances = _state.Instances.Select(i => i with { CuePending = false }).ToArray() };
        return _state;
    }
    private Task Delay(TimeSpan span, CancellationToken cancellation)
    {
        _delays.Add(span);
        if (_heldDelays.TryGetValue(span, out var held)) return held.Task.WaitAsync(cancellation);
        return Task.CompletedTask;
    }
    private (GraphicsWorkspaceLibrary Library, GraphicsPlayoutService Service) Build()
    {
        var session = new GraphicsWorkspaceSession(Invoke, Observe); session.Observe(_state);
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(Path.Combine(_directory, "show.json")), session);
        return (library, new GraphicsPlayoutService(library, Invoke, Observe, Delay, () => DateTime.UtcNow));
    }
    private async Task<(GraphicsWorkspaceLibrary Library, GraphicsPlayoutService Service)> BuildWithDefaultPlaylist()
    {
        var built = Build();
        Assert.True(await built.Library.SavePlaylistAsync(GraphicsWorkspaceLibrary.DefaultPlaylistFor(GraphicsShowStarters.All[0], _state), GraphicsWorkspaceLibrary.DefaultShotLayouts()), built.Library.Status);
        return built;
    }
    private readonly List<string> _sceneSelects = [];
    private Task<bool> SelectScene(string sceneId, CancellationToken _) { _sceneSelects.Add(sceneId); return Task.FromResult(sceneId is "intro" or "interview" or "panel"); }
    private Task<NativeGraphicsOperationResult> Invoke(string action, string body, CancellationToken cancellation)
    {
        var input = JsonDocument.Parse(body).RootElement.Clone(); lock (_commands) _commands.Add((action, input));
        var id = input.TryGetProperty("instanceId", out var rawId) ? rawId.GetString()! : "";
        long revision = 1;
        NativeGraphicsInstance[] Map(Func<NativeGraphicsInstance, NativeGraphicsInstance> change) => _state.Instances.Select(i => i.InstanceId == id ? change(i) : i).ToArray();
        switch (action) {
            case "graphics.instance.create":
                var binding = input.TryGetProperty("binding", out var b) && b.ValueKind == JsonValueKind.Object ? JsonSerializer.Deserialize<SourceInstanceIdentity>(b, Json) : null;
                _state = _state with { Instances = [.. _state.Instances, new() { InstanceId = id, Revision = 1, TemplateId = input.GetProperty("templateId").GetString()!, TemplateVersion = input.GetProperty("templateVersion").GetInt32(), Scope = "global",
                    Animations = JsonSerializer.Deserialize<NativeGraphicsAnimations>(input.GetProperty("animations"), Json)!,
                    Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(input.GetProperty("fields"), Json)!, Binding = binding, SourceId = binding?.SourceId ?? "" }] };
                break;
            case "graphics.instance.update":
                revision = _state.Instances.Single(i => i.InstanceId == id).Revision + 1;
                _state = _state with { Instances = Map(i => i with { Revision = revision, Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(input.GetProperty("fields"), Json)!,
                    ProgramRevision = input.GetProperty("target").GetString() == "program" ? revision : i.ProgramRevision,
                    TemplateVersion = input.TryGetProperty("templateVersion", out var version) ? version.GetInt32() : i.TemplateVersion,
                    Animations = input.TryGetProperty("animations", out var animations) ? JsonSerializer.Deserialize<NativeGraphicsAnimations>(animations, Json)! : i.Animations }) };
                break;
            case "graphics.instance.cue":
                var expected = input.GetProperty("expectedRevision").GetInt64();
                _state = _state with { Instances = Map(i => i with { CueRevision = expected, CuePending = _cuePendingPolls > 0 || id == _holdCueInstance, Cued = true, CueOwner = input.TryGetProperty("cueOwner", out var owner) ? owner.GetString()! : "" }) };
                if (id == _holdCueInstance) _heldCueReached.TrySetResult();
                if (id == _lostCueReply) throw new InvalidOperationException("Cue response was lost.");
                break;
            case "graphics.instance.trigger":
                _state = input.GetProperty("action").GetString() switch {
                    "show" => _showDoesNotLand ? _state : _state with { Instances = Map(i => i with { ProgramRevision = i.CueRevision, ProgramTemplateVersion = i.TemplateVersion, Visibility = "visible", CueRevision = 0, Cued = false }) },
                    "hide" => _state with { Instances = Map(i => i with { ProgramRevision = 0, Visibility = "hidden" }) },
                    "uncue" => _state with { Instances = Map(i => !input.TryGetProperty("cueOwner", out var cancelOwner) || i.CueOwner == cancelOwner.GetString()
                        ? i with { CueRevision = 0, Cued = false, CuePending = false, CueOwner = "" } : i) },
                    _ => _state };
                break;
            case "graphics.instance.remove": _state = _state with { Instances = _state.Instances.Where(i => i.InstanceId != id).ToArray() }; break;
            case "graphics.template.install": {
                // The native library holds 16 definitions; a full one fails the install record asynchronously.
                var requestId = input.GetProperty("requestId").GetString()!;
                var manifest = JsonDocument.Parse(input.GetProperty("manifest").GetString()!).RootElement;
                var templateId = manifest.GetProperty("templateId").GetString()!;
                _state = _state.Templates.Count >= 16
                    ? _state with { TemplateInstalls = [.. _state.TemplateInstalls, new() { RequestId = requestId, State = "failed", Reason = "template-library-capacity" }] }
                    : _state with { Templates = [.. _state.Templates, Template(templateId, "lower-third", "declarative") with { TemplateVersion = manifest.GetProperty("templateVersion").GetInt32() }] };
                break;
            }
            case "graphics.template.remove": {
                var templateId = input.GetProperty("templateId").GetString()!; var version = input.GetProperty("templateVersion").GetInt32();
                if (!_state.Templates.Any(t => t.TemplateId == templateId && t.TemplateVersion == version))
                    return Task.FromResult(new NativeGraphicsOperationResult { Reason = "template-not-found" });
                _state = _state with { Templates = _state.Templates.Where(t => !(t.TemplateId == templateId && t.TemplateVersion == version)).ToArray() };
                break;
            }
        }
        return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = id, CurrentRevision = revision, Reason = "accepted" });
    }
    // Auto-out hides run on a pool thread, so readers take a locked snapshot.
    private (string Action, JsonElement Body)[] Commands { get { lock (_commands) return [.. _commands]; } }
    private void ClearCommands() { lock (_commands) _commands.Clear(); }
    private IEnumerable<string> Actions => Commands.Select(c => c.Action);
    private IEnumerable<JsonElement> Bodies(string action) => Commands.Where(c => c.Action == action).Select(c => c.Body);

    [Fact] public async Task PlaylistReinstallEvictsUnusedDesignVersionsWhenTheLibraryIsFull()
    {
        const string pinned = "design-guest-a1b2c3";
        var row = new GraphicsPlaylistRow { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest name", TemplateId = pinned, TemplateVersion = 1 };
        // The row's pin is in the document but the design is not installed (e.g. after a core restart).
        await new GraphicsDocumentStore(Path.Combine(_directory, "show.json")).SaveAsync(new GraphicsShowDocument {
            Playlist = [row], Templates = [new GraphicsSavedTemplate(pinned, 1, Hash(pinned), JsonSerializer.SerializeToElement(new { templateId = pinned, templateVersion = 1 }))] });
        // 6 curated + 10 design versions nothing references = a full 16-definition library.
        _state = _state with { Templates = [.. _state.Templates, .. Enumerable.Range(0, 10).Select(i => Template($"design-bug-{i:x6}", "bug", "declarative"))] };
        var (_, service) = Build();
        var result = await service.EnsureInstanceAsync(GraphicsPlayoutService.EntryFor(row));
        Assert.True(result.Accepted, result.Reason);
        Assert.Contains(Commands, c => c.Action == "graphics.template.remove");
        Assert.DoesNotContain(Commands, c => c.Action == "graphics.template.remove" && c.Body.GetProperty("templateId").GetString() == pinned);
        Assert.DoesNotContain(Commands, c => c.Action == "graphics.template.remove" && !c.Body.GetProperty("templateId").GetString()!.StartsWith("design-", StringComparison.Ordinal));
        Assert.Contains(_state.Templates, t => t.TemplateId == pinned && t.TemplateVersion == 1);
        Assert.Contains(_state.Instances, i => i.TemplateId == pinned);
    }

    [Fact] public async Task DefaultPlaylistUsesCuratedTemplatesAndAssignsF1ToF5()
    {
        var starter = GraphicsShowStarters.All[0];
        var rows = GraphicsWorkspaceLibrary.DefaultPlaylistFor(starter, _state);
        Assert.Equal(new[] { "guest", "title", "logo", "live-bug", "sponsor" }, rows.Select(r => r.Id));
        Assert.Equal(new[] { "F1", "F2", "F3", "F4", "F5" }, rows.Select(r => r.Hotkey));
        Assert.Equal(new[] { GraphicsPlaylistRow.Guest, GraphicsPlaylistRow.Title, GraphicsPlaylistRow.Logo, GraphicsPlaylistRow.LiveBug, GraphicsPlaylistRow.Sponsor }, rows.Select(r => r.Kind));
        Assert.Equal(new[] { starter.LowerThird, "centered-title", "corner-logo", "clock-panel", "" }, rows.Select(r => r.TemplateId));
        Assert.True(rows[0].FollowSources); Assert.Equal(8000, rows[0].PlayOutAfterMs); Assert.Equal(starter.Accent, rows[0].Style.AccentColor);
        Assert.Equal(starter.Heading, rows[1].Fields["heading"].GetString());
        Assert.Equal("LIVE", rows[3].Fields["label"].GetString()); Assert.True(rows[3].ShowClock);
        Assert.Equal(15000, rows[4].PlayOutAfterMs); Assert.Null(rows[4].Image); Assert.Null(rows[4].ImagePath);
        Assert.All(rows, r => Assert.True(r.IsValid));
        var (library, _) = Build();
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        Assert.Contains(library.Document.Templates, t => t.TemplateId == "centered-title" && t.ManifestHash == Hash("centered-title"));
        var loaded = await new GraphicsDocumentStore(Path.Combine(_directory, "show.json")).LoadAsync();
        Assert.Equal(rows.Select(r => (r.Id, r.Hotkey, r.TemplateId, r.TemplateVersion)), loaded.Document.Playlist.Select(r => (r.Id, r.Hotkey, r.TemplateId, r.TemplateVersion)));
        Assert.Empty(Commands);
    }

    [Fact] public async Task PlayInSendsTheRowsOwnMotionWhenItHasOne()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var rows = library.Document.Playlist.ToList();
        rows[1] = rows[1] with { Motion = GraphicsBuilder.MotionFor("slide-right", 150) };
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        var animations = Bodies("graphics.instance.create").Single().GetProperty("animations");
        Assert.Equal("slide", animations.GetProperty("in").GetProperty("type").GetString());
        Assert.Equal("right", animations.GetProperty("in").GetProperty("direction").GetString());
        Assert.Equal(150, animations.GetProperty("in").GetProperty("durationMs").GetDouble());
        ClearCommands();
        // A row without its own motion still gets the playout default.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        Assert.Equal("fade", Bodies("graphics.instance.create").Single().GetProperty("animations").GetProperty("in").GetProperty("type").GetString());
    }

    [Fact] public async Task PlayInOfANamesListVariantBindsThatLinesNameAndTitle()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var rows = library.Document.Playlist.ToList();
        var guest = rows[0]; // lower-third template row
        rows[0] = guest with { Kind = GraphicsPlaylistRow.Title, FollowSources = false, Variants = GraphicsBuilder.ParseNamesList("Ada Lovelace, Analyst\nAlan Turing, Mathematician") };
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        var saved = library.Document.Playlist[0];
        var entries = service.GuestEntries([], (_, _) => ("", ""));
        Assert.Equal(["variant:ada-lovelace", "variant:alan-turing"], entries.Where(e => e.RowId == saved.Id).Select(e => e.SourceId));
        Assert.True((await service.PlayInAsync(entries[1])).Accepted);
        var create = Bodies("graphics.instance.create").Single();
        Assert.Equal(GraphicsPlayoutService.InstanceIdFor(saved.Id, "variant:alan-turing"), create.GetProperty("instanceId").GetString());
        Assert.Equal("Alan Turing", create.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("Mathematician", create.GetProperty("fields").GetProperty("title").GetString());
        Assert.False(create.TryGetProperty("binding", out _));
    }

    // Acceptance 1 (spec §4): a design with an extra variable, saved as a Guest row that follows
    // Sources; the operator types the extra value on the row and Show sends it beside the Sources text.
    [Fact] public async Task ShowOfAGuestDesignRowSendsTheTypedExtraVariableBesideTheSourcesText()
    {
        const string design = "design-lower-third-a1b2c3";
        _state = _state with { Templates = [.. _state.Templates, Template(design, "lower-third", "declarative", "name", "title", "location")] };
        var (library, service) = Build();
        var row = new GraphicsPlaylistRow { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest", FollowSources = true, TemplateId = design, TemplateVersion = 1, Hotkey = "F1",
            Fields = new Dictionary<string, JsonElement> { ["name"] = JsonSerializer.SerializeToElement("Name"), ["title"] = JsonSerializer.SerializeToElement("Title"), ["location"] = JsonSerializer.SerializeToElement("") } };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        var saved = library.Document.Playlist.Single();
        Assert.Equal(["location"], GraphicsRowFields.Typeable(saved, _state, library.Document.Templates).Select(f => f.Id));
        Assert.True(await library.SavePlaylistAsync([GraphicsRowFields.WithText(saved, "location", "Oslo")]), library.Status);
        var entries = service.GuestEntries([new(1, "zoom:7", "Ada")], (_, name) => (name, "Analyst"));
        Assert.True((await service.PlayInAsync(entries.Single())).Accepted);
        var fields = Bodies("graphics.instance.create").Single().GetProperty("fields");
        Assert.Equal("Ada", fields.GetProperty("name").GetString());
        Assert.Equal("Analyst", fields.GetProperty("title").GetString());
        Assert.Equal("Oslo", fields.GetProperty("location").GetString());
    }


    [Fact]
    public async Task ShowAfterReloadUsesEachGuestsColumnAndNamesListCell()
    {
        const string design = "design-lower-third-a1b2c3";
        _state = _state with { Templates = [.. _state.Templates, Template(design, "lower-third", "declarative", "name", "title", "location")] };
        var (library, _) = Build();
        Assert.True(await library.AddSourceColumnAsync("office"));
        Assert.True(await library.SaveSourceValuesAsync(1, new Dictionary<string, string> { ["office"] = "Boston" }));
        Assert.True(await library.SaveSourceValuesAsync(2, new Dictionary<string, string> { ["office"] = "London" }));
        var guest = new GraphicsPlaylistRow { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest",
            FollowSources = true, TemplateId = design, TemplateVersion = 1,
            SourceMappings = new Dictionary<string, string> { ["location"] = "office" } };
        var list = guest with { Id = "names", Kind = GraphicsPlaylistRow.Title, FollowSources = false,
            Variants = GraphicsBuilder.ParseNamesList("name,title,location\nAda,Analyst,\"Paris, France\"\nAlan,Engineer,Tokyo") };
        Assert.True(await library.SavePlaylistAsync([guest, list]), library.Status);

        var (reopened, service) = Build();
        await reopened.LoadAsync();
        var entries = service.GuestEntries([new(1, "zoom:7", "Ada"), new(2, "capture:camera", "Alan")], (_, name) => (name, ""));
        foreach (var entry in entries)
            Assert.True((await service.PlayInAsync(entry)).Accepted);
        Assert.Equal(["Boston", "London", "Paris, France", "Tokyo"],
            Bodies("graphics.instance.create").Select(body => body.GetProperty("fields").GetProperty("location").GetString()));
    }

    [Fact]
    public async Task LocalLeaderboardSurvivesReopenAndPublishesWithoutAnotherShow()
    {
        const string design = "design-scores";
        var first = GraphicsLocalData.Parse("id,player,score\na,Ada,10\nb,Bob,9", "csv", "id");
        var binding = new GraphicsDataBinding("scores", "id", ["player", "score"], "score", 2, 1);
        var fields = GraphicsDataTable.Fields(first, binding);
        _state = _state with { Templates = [.. _state.Templates, Template(design, "title", "declarative", fields.Keys.ToArray())] };
        var (library, _) = Build();
        Assert.NotNull(await library.SaveLocalDataAsync("scores", "Scores", first));
        var row = new GraphicsPlaylistRow { Id = "scores", Kind = GraphicsPlaylistRow.Title, Name = "Scores",
            TemplateId = design, TemplateVersion = 1, DataBinding = binding, Fields = fields };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        var (reopened, service) = Build();
        await reopened.LoadAsync();
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(row))).Accepted);
        Assert.Contains("Ada", Bodies("graphics.instance.create").Single().GetProperty("fields").GetProperty("table-row-1").GetString());
        Assert.NotNull(await reopened.SaveLocalDataAsync("scores", "Scores",
            GraphicsLocalData.Parse("id,player,score\na,Ada,10\nb,Bob,19", "csv", "id")));
        Assert.Single(Bodies("graphics.instance.trigger"));
        Assert.True((await service.PublishDataAsync(row.Id)).Accepted);
        var update = Bodies("graphics.instance.update").Single();
        Assert.Equal("program", update.GetProperty("target").GetString());
        Assert.Contains("Bob", update.GetProperty("fields").GetProperty("table-row-1").GetString());
        Assert.Single(Bodies("graphics.instance.trigger"));
        Assert.Equal(2, reopened.Document.Playlist.Single().DataBinding!.Revision);
    }

    [Fact]
    public async Task SavedCrawlCuesPinnedDataAndPublishesWithoutReshowingProgram()
    {
        const string design = "design-crawl";
        var first = GraphicsLocalData.Parse("line\nFirst\nSecond", "csv", null);
        var binding = new GraphicsDataBinding("crawl", null, ["line"], null, 64, 1) { Crawl = new() };
        var fields = GraphicsDataTable.Fields(first, binding);
        _state = _state with { Templates = [.. _state.Templates, Template(design, "title", "declarative", fields.Keys.ToArray())] };
        var (library, _) = Build();
        Assert.NotNull(await library.SaveLocalDataAsync("crawl", "Crawl", first));
        var row = new GraphicsPlaylistRow { Id = "crawl", Kind = GraphicsPlaylistRow.Title, Name = "Crawl",
            TemplateId = design, TemplateVersion = 1, DataBinding = binding, Fields = fields };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        var (reopened, service) = Build();
        await reopened.LoadAsync();
        var cue = await service.ToPreviewAsync(GraphicsPlayoutService.EntryFor(reopened.Document.Playlist.Single()));
        Assert.True(cue.Accepted, cue.Reason);
        var create = Bodies("graphics.instance.create").Single();
        Assert.Equal(1, create.GetProperty("templateVersion").GetInt32());
        Assert.Equal("1", create.GetProperty("fields").GetProperty(GraphicsCrawlData.RevisionField).GetString());
        Assert.Empty(Bodies("graphics.instance.trigger"));

        // The fake core observes the real native Take result. Private and
        // Program clock independence is exercised in GraphicsInstancesTest.
        _state = _state with { Instances = _state.Instances.Select(instance => instance with
        {
            Cued = false, CueRevision = 0, ProgramRevision = instance.Revision, ProgramTemplateVersion = 1,
            Visibility = "on-air", CrawlPlayback = "playing", ProgramFields = instance.Fields
        }).ToArray() };
        Assert.True((await service.ControlCrawlAsync(row.Id, "pause")).Accepted);
        Assert.Equal("pause", Bodies("graphics.instance.trigger").Single().GetProperty("action").GetString());
        var currentBinding = service.Row(row.Id)!.DataBinding!;
        var manual = await service.StageDataAsync(row.Id, currentBinding, GraphicsLocalData.Parse("line\nRevised", "csv", null));
        Assert.True(manual.Accepted, manual.Reason);
        Assert.Empty(Bodies("graphics.instance.update"));
        Assert.Equal(1, service.Row(row.Id)!.DataBinding!.Revision);
        Assert.True((await service.PublishDataAsync(row.Id)).Accepted);
        var update = Bodies("graphics.instance.update").Single();
        Assert.Equal("program", update.GetProperty("target").GetString());
        Assert.Equal("2", update.GetProperty("fields").GetProperty(GraphicsCrawlData.RevisionField).GetString());
        Assert.Single(Bodies("graphics.instance.trigger"));
        Assert.True((await service.SetDataLiveFollowAsync(row.Id, true)).Accepted);
        currentBinding = service.Row(row.Id)!.DataBinding!;
        var rejected = await service.StageDataAsync(row.Id, currentBinding, GraphicsLocalData.Parse("wrong\nUnusable", "csv", null));
        Assert.False(rejected.Accepted);
        Assert.Equal(2, reopened.Document.DataSources.Single().Revision);
        Assert.Single(Bodies("graphics.instance.update"));
        var followed = await service.StageDataAsync(row.Id, currentBinding, GraphicsLocalData.Parse("line\nFollowed", "csv", null));
        Assert.True(followed.Accepted, followed.Reason);
        Assert.Equal("3", Bodies("graphics.instance.update").Last().GetProperty("fields").GetProperty(GraphicsCrawlData.RevisionField).GetString());
        Assert.Equal(2, Bodies("graphics.instance.update").Count());
        var (later, _) = Build();
        await later.LoadAsync();
        Assert.True(later.Document.Playlist.Single().DataBinding!.LiveFollow);
        Assert.Equal(3, later.Document.Playlist.Single().DataBinding!.Revision);
        Assert.True((await service.PlayOutAsync(cue.InstanceId)).Accepted);
        Assert.Equal("hide", Bodies("graphics.instance.trigger").Last().GetProperty("action").GetString());
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RemoteFeedKeepsCuePinnedAndFollowsOnlyAfterOptIn(bool crawl)
    {
        const string design = "design-remote";
        var data = GraphicsLocalData.Parse("id,line\na,First", "csv", "id");
        var next = GraphicsLocalData.Parse("id,line\na,Second", "csv", "id");
        var later = GraphicsLocalData.Parse("id,line\na,Third", "csv", "id");
        var binding = new GraphicsDataBinding("feed", "id", ["line"], null, 8, 1) { Crawl = crawl ? new() : null, Remote = true };
        var fields = GraphicsDataTable.Fields(data, binding);
        _state = _state with { Templates = [.. _state.Templates, Template(design, "title", "declarative", fields.Keys.ToArray())] };
        var (library, service) = Build();
        var definition = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(
            new("feed", "Feed", "json", "https://example.com/feed", "id", Enabled: true)));
        var now = DateTimeOffset.UtcNow;
        Assert.Equal(GraphicsFeedStage.Updated, await service.StageFeedAsync(definition, data, now, default));
        var row = new GraphicsPlaylistRow { Id = "feed", Kind = GraphicsPlaylistRow.Title, Name = "Feed",
            TemplateId = design, TemplateVersion = 1, DataBinding = binding, Fields = fields };
        Assert.True(await library.SavePlaylistAsync([row]));
        var cue = await service.ToPreviewAsync(GraphicsPlayoutService.EntryFor(row));
        Assert.True(cue.Accepted, cue.Reason);
        Assert.Equal(GraphicsFeedStage.Updated, await service.StageFeedAsync(definition, next, now.AddSeconds(15), default));
        Assert.Equal(1, library.Document.Playlist.Single().DataBinding!.Revision);
        Assert.Empty(Bodies("graphics.instance.update"));
        Assert.Equal(1, _state.Instances.Single().CueRevision);
        Assert.True((await service.PublishDataAsync(row.Id)).Accepted);
        Assert.Empty(Bodies("graphics.instance.update"));
        Assert.Equal(1, _state.Instances.Single().CueRevision);
        _state = _state with { Instances = _state.Instances.Select(instance => instance with
        {
            ProgramRevision = instance.CueRevision, ProgramTemplateVersion = 1, Visibility = "on-air"
        }).ToArray() };
        var saved = library.Document.Playlist.Single();
        Assert.True(await library.SavePlaylistAsync([saved with { DataBinding = saved.DataBinding! with { LiveFollow = true } }]));
        Assert.Equal(GraphicsFeedStage.Updated, await service.StageFeedAsync(definition, later, now.AddSeconds(30), default));
        Assert.Equal(3, library.Document.Playlist.Single().DataBinding!.Revision);
        Assert.Equal("program", Assert.Single(Bodies("graphics.instance.update")).GetProperty("target").GetString());
        Assert.Equal(1, _state.Instances.Single().CueRevision);
        Assert.Empty(Bodies("graphics.instance.trigger"));

        var disabled = definition with { Enabled = false };
        Assert.NotNull(await library.SaveFeedAsync(disabled));
        Assert.Equal(GraphicsFeedStage.Rejected, await service.StageFeedAsync(definition, next, now.AddSeconds(45), default));
        Assert.Equal(3, library.Document.DataSources.Single().Revision);
        Assert.Single(Bodies("graphics.instance.update"));
    }

    [Fact] public async Task PlayInSendsTheRowsOwnPlacementWhenItHasOne()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var rows = library.Document.Playlist.ToList();
        rows[1] = rows[1] with { Placement = new() { Anchor = "lower-right", X = -40, Y = -30, Scale = .75 } };
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        var placement = Bodies("graphics.instance.create").Single().GetProperty("placement");
        Assert.Equal("lower-right", placement.GetProperty("anchor").GetString());
        Assert.Equal(.75, placement.GetProperty("scale").GetDouble());
    }

    [Fact] public async Task AClockRowConfiguresItsNativeClockAfterCreateAndOffersTransport()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var rows = library.Document.Playlist.ToList();
        rows[3] = rows[3] with { Clock = new() { Mode = "countdown", DurationSeconds = 120 } }; // LIVE bug row (clock template)
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        var entry = GraphicsPlayoutService.EntryFor(library.Document.Playlist[3]);
        Assert.True((await service.ClockAsync(entry, "start")).Accepted == false, "transport before an instance exists must be refused");
        Assert.True((await service.ToPreviewAsync(entry)).Accepted);
        var configure = Bodies("graphics.clock.control").Single();
        Assert.Equal("configure", configure.GetProperty("action").GetString());
        Assert.Equal(1, configure.GetProperty("expectedClockRevision").GetInt64()); // the core's initial clock revision
        Assert.Equal(1, configure.GetProperty("expectedRevision").GetInt64());
        Assert.Equal("countdown", configure.GetProperty("configuration").GetProperty("mode").GetString());
        Assert.Equal(120, configure.GetProperty("configuration").GetProperty("durationSeconds").GetInt32());
        ClearCommands();
        Assert.True((await service.ClockAsync(entry, "start")).Accepted);
        var start = Bodies("graphics.clock.control").Single();
        Assert.Equal("start", start.GetProperty("action").GetString()); Assert.True(start.GetProperty("expectedRevision").GetInt64() >= 1);
        Assert.False((await service.ClockAsync(entry, "explode")).Accepted);
    }

    [Fact] public async Task StripChipsOfANamesListCarryTheLineSoTheNextShowSteps()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var rows = library.Document.Playlist.ToList();
        rows[0] = rows[0] with { Kind = GraphicsPlaylistRow.Title, FollowSources = false, PlayOutAfterMs = 0, Variants = GraphicsBuilder.ParseNamesList("Ada, One\nAlan, Two") };
        Assert.True(await library.SavePlaylistAsync(rows), library.Status);
        var row = library.Document.Playlist[0];
        var entries = GraphicsPlayoutService.VariantEntries(row);
        Assert.True((await service.PlayInAsync(entries[0])).Accepted);
        var chip = Assert.Single(service.Strip.OnProgram);
        Assert.Equal("variant:ada", chip.SourceId); Assert.Equal(row.Name + " \u00B7 Ada", chip.Label);
        Assert.Equal("variant:alan", GraphicsPlayoutService.NextVariant(row, service.Strip.OnProgram.Select(i => i.SourceId))!.SourceId);
        Assert.Equal("", GraphicsPlayoutService.VariantSourceIdOf(row, "playlist-title"));
        // Showing the next line replaces the previous one; another row's graphic is untouched.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        ClearCommands();
        Assert.True((await service.PlayInAsync(entries[1])).Accepted);
        var hides = Commands.Where(c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide").Select(c => c.Body.GetProperty("instanceId").GetString()).ToArray();
        Assert.Equal([GraphicsPlayoutService.InstanceIdFor(row.Id, "variant:ada")], hides);
        Assert.Equal(["variant:alan", ""], service.Strip.OnProgram.Select(i => i.SourceId).OrderBy(s => s.Length == 0).ToArray());
    }

    [Fact] public async Task PlayInCreatesCuesWaitsThenShowsFromCue()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        _cuePendingPolls = 2;
        var result = await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]));
        Assert.True(result.Accepted, result.Reason);
        Assert.Equal(new[] { "graphics.instance.create", "graphics.instance.cue", "graphics.instance.trigger" }, Actions);
        var create = Bodies("graphics.instance.create").Single();
        Assert.Equal("playlist-title", create.GetProperty("instanceId").GetString());
        Assert.Equal("centered-title", create.GetProperty("templateId").GetString());
        Assert.Equal(GraphicsShowStarters.All[0].Heading, create.GetProperty("fields").GetProperty("heading").GetString());
        Assert.Equal(GraphicsPlayoutService.ClientId, create.GetProperty("clientId").GetString());
        var cue = Bodies("graphics.instance.cue").Single();
        Assert.True(cue.GetProperty("rideTake").GetBoolean()); Assert.Equal(1, cue.GetProperty("expectedRevision").GetInt64());
        var trigger = Bodies("graphics.instance.trigger").Single();
        Assert.Equal("show", trigger.GetProperty("action").GetString()); Assert.Equal("cue", trigger.GetProperty("target").GetString());
        Assert.Contains(TimeSpan.FromMilliseconds(50), _delays);
        Assert.Equal(0, _cuePendingPolls);
        Assert.Equal(1, _state.Instances.Single(i => i.InstanceId == "playlist-title").ProgramRevision);
        Assert.Single(service.Strip.OnProgram); Assert.Empty(service.Strip.OnPreview);
        ClearCommands();
        Assert.True((await service.ToPreviewAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        Assert.Equal(new[] { "graphics.instance.create", "graphics.instance.cue" }, Actions);
        Assert.Equal(new[] { "playlist-logo" }, service.Strip.OnPreview.Select(i => i.InstanceId));
    }

    [Fact] public async Task PlayOutHidesExactlyTheTargetInstance()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        Assert.Equal(2, service.Strip.OnProgram.Count); ClearCommands();
        var result = await service.PlayOutAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]));
        Assert.True(result.Accepted, result.Reason);
        var hide = Assert.Single(Commands);
        Assert.Equal("graphics.instance.trigger", hide.Action); Assert.Equal("hide", hide.Body.GetProperty("action").GetString());
        Assert.Equal("playlist-logo", hide.Body.GetProperty("instanceId").GetString()); Assert.False(hide.Body.TryGetProperty("target", out _));
        Assert.Equal(new[] { "playlist-title" }, service.Strip.OnProgram.Select(i => i.InstanceId));
        Assert.False((await service.PlayOutAsync("playlist-logo")).Accepted); Assert.Single(Commands);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted); ClearCommands();
        Assert.True((await service.PlayOutAllAsync()).Accepted);
        Assert.Equal(new[] { "playlist-title", "playlist-logo" }.OrderBy(x => x), Bodies("graphics.instance.trigger").Select(b => b.GetProperty("instanceId").GetString()).OrderBy(x => x));
        Assert.Empty(service.Strip.OnProgram);
    }

    [Fact] public async Task StripOnProgramReflectsNativeProgramRevisionNotLocalIntent()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        _showDoesNotLand = true; var changes = 0; service.StripChanged += (_, _) => changes++;
        var result = await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]));
        Assert.True(result.Accepted);
        Assert.Empty(service.Strip.OnProgram);
        Assert.Equal(new[] { "playlist-title" }, service.Strip.OnPreview.Select(i => i.InstanceId));
        _state = _state with { Instances = _state.Instances.Select(i => i with { ProgramRevision = i.CueRevision, Visibility = "building-in", CueRevision = 0 }).ToArray() };
        service.Observe(_state);
        var chip = Assert.Single(service.Strip.OnProgram);
        Assert.Equal(("playlist-title", "playlist", "title", "Show title"), (chip.InstanceId, chip.Owner, chip.RowId, chip.Label));
        Assert.Empty(service.Strip.OnPreview);
        _state = _state with { Instances = _state.Instances.Select(i => i with { Visibility = "building-out" }).ToArray() };
        var before = changes; service.Observe(_state); service.Observe(_state);
        Assert.Empty(service.Strip.OnProgram); Assert.Equal(before + 1, changes);
    }

    [Fact] public async Task StripOnPreviewEqualsCuedInstances()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var guestId = GraphicsPlayoutService.InstanceIdFor("guest", "zoom:7");
        _state = _state with {
            Instances = [
                new() { InstanceId = "playlist-title", Revision = 2, CueRevision = 2, TemplateId = "centered-title" },
                new() { InstanceId = guestId, Revision = 3, CueRevision = 3, TemplateId = "minimal-accent", Fields = new() { Name = "Ada" }, Binding = Identity("zoom:7") },
                new() { InstanceId = "graphic-design", Label = "Designer cue", Revision = 1, CueRevision = 1, CuePending = true, TemplateId = "compact-solid" },
                new() { InstanceId = "graphic-auto", Label = "Auto LT", Revision = 4, ProgramRevision = 4, Visibility = "visible", TemplateId = "compact-solid" },
                new() { InstanceId = "graphic-idle", Revision = 1, TemplateId = "compact-solid" }],
            Automation = new() { Mode = "automatic", Armed = true, Configuration = new() { Rules = [new() { Id = "guest-introduction", InstanceId = "graphic-auto" }] } }
        };
        service.Observe(_state);
        Assert.Equal(new[] { "playlist-title", guestId, "graphic-design" }, service.Strip.OnPreview.Select(i => i.InstanceId));
        Assert.Equal(new[] { "playlist", "playlist", "design" }, service.Strip.OnPreview.Select(i => i.Owner));
        Assert.Equal(("guest", "zoom:7", "Guest name · Ada"), (service.Strip.OnPreview[1].RowId, service.Strip.OnPreview[1].SourceId, service.Strip.OnPreview[1].Label));
        Assert.True(service.Strip.OnPreview[2].CuePending);
        var program = Assert.Single(service.Strip.OnProgram);
        Assert.Equal(("graphic-auto", "auto", "guest-introduction"), (program.InstanceId, program.Owner, program.RuleId));
        Assert.Equal(new[] { "graphic-auto" }, service.Strip.Auto.Select(i => i.InstanceId));
        Assert.Equal("automatic", service.Strip.AutomationMode); Assert.True(service.Strip.AutomationArmed);
        Assert.True((await service.RemoveFromPreviewAsync("graphic-design")).Accepted);
        var uncue = Assert.Single(Commands);
        Assert.Equal(("graphics.instance.trigger", "uncue", "graphic-design"), (uncue.Action, uncue.Body.GetProperty("action").GetString(), uncue.Body.GetProperty("instanceId").GetString()));
        Assert.Equal(new[] { "playlist-title", guestId }, service.Strip.OnPreview.Select(i => i.InstanceId));
        ClearCommands();
        Assert.True((await service.ClearPreviewAsync()).Accepted);
        Assert.Equal(2, Commands.Length); Assert.All(Commands, c => Assert.Equal("uncue", c.Body.GetProperty("action").GetString()));
        Assert.Empty(service.Strip.OnPreview); Assert.Single(service.Strip.OnProgram);
        Assert.False((await service.RemoveFromPreviewAsync("graphic-idle")).Accepted);
    }

    [Fact] public async Task AutoOutFiresOnceAfterConfiguredMs()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var held = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _heldDelays[TimeSpan.FromMilliseconds(8000)] = held;
        var entry = service.GuestEntryFor("zoom:7", [new(1, "zoom:7", "Ada")], (_, name) => (name + " Lovelace", "Guest"))!;
        var result = await service.PlayInAsync(entry);
        Assert.True(result.Accepted, result.Reason);
        var instanceId = GraphicsPlayoutService.InstanceIdFor(entry);
        var create = Bodies("graphics.instance.create").Single();
        Assert.Equal(instanceId, create.GetProperty("instanceId").GetString());
        Assert.Equal("Ada Lovelace", create.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("zoom:7", create.GetProperty("binding").GetProperty("sourceId").GetString());
        Assert.Contains(TimeSpan.FromMilliseconds(8000), _delays);
        Assert.DoesNotContain(Commands, c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide");
        Assert.True(Assert.Single(service.Strip.OnProgram).AutoOutArmed);
        held.SetResult();
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (_state.Instances.Single(i => i.InstanceId == instanceId).ProgramRevision > 0 && DateTime.UtcNow < deadline) await Task.Delay(5);
        var hides = Commands.Where(c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide").ToArray();
        Assert.Equal(instanceId, Assert.Single(hides).Body.GetProperty("instanceId").GetString());
        Assert.Empty(service.Strip.OnProgram);
        held.TrySetResult(); service.Observe(_state); await Task.Delay(20);
        Assert.Single(Commands, c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide");
        // Title row: PlayOutAfterMs 0 never arms a timer.
        _delays.Clear();
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.DoesNotContain(_delays, d => d >= TimeSpan.FromSeconds(1)); Assert.False(service.Strip.OnProgram.Single().AutoOutArmed);
    }

    [Fact] public async Task GuestEntriesEnumerateEveryBoundSlotIncludingCapture()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        GraphicsRosterSlot[] slots = [new(3, "media:clip-1", "Opening clip"), new(1, "zoom:7", "Ada"), new(2, "capture:camera", "HD60 S+"), new(4, null, ""), new(5, "", ""), new(6, "zoom:7", "Ada again")];
        var entries = service.GuestEntries(slots, (id, name) => (name.ToUpperInvariant(), "Title for " + id));
        Assert.Equal(new[] { "zoom:7", "capture:camera", "media:clip-1" }, entries.Select(e => e.SourceId));
        Assert.All(entries, e => Assert.Equal("guest", e.RowId));
        Assert.Equal(new[] { "ADA", "HD60 S+", "OPENING CLIP" }, entries.Select(e => e.Label));
        Assert.Equal("Title for capture:camera", entries[1].Title);
        var ids = entries.Select(GraphicsPlayoutService.InstanceIdFor).ToArray();
        Assert.Equal(3, ids.Distinct().Count());
        Assert.All(ids, id => { Assert.StartsWith("playlist-guest-", id); Assert.Equal(12, id["playlist-guest-".Length..].Length); Assert.All(id, c => Assert.True(c is >= 'a' and <= 'z' or >= '0' and <= '9' or '-')); });
        Assert.Equal(ids[0], GraphicsPlayoutService.InstanceIdFor("guest", "zoom:7"));
        Assert.Equal("playlist-title", GraphicsPlayoutService.InstanceIdFor(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1])));
        // A capture guest plays in with its own instance and operator text; no roster fill of unbound sources.
        Assert.True((await service.PlayInAsync(entries[1])).Accepted);
        var create = Bodies("graphics.instance.create").Single();
        Assert.Equal(ids[1], create.GetProperty("instanceId").GetString());
        Assert.Equal("HD60 S+", create.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("capture:camera", create.GetProperty("binding").GetProperty("sourceId").GetString());
        Assert.Empty(GraphicsPlayoutService.GuestEntries(library.Document.Playlist[0], [new(1, null, "")], (_, n) => (n, "")));
    }

    // Stage C1 (#1076): the legacy "LT in/out" intent (footer button, graphics.lowerThird.*)
    // routes into the Guest row for the current Program subject; authority flips to instances
    // only after the first ACCEPTED Play In.
    [Fact] public async Task LegacyLowerThirdIntentMapsToGuestRowOfProgramSubject()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.True(service.HasGuestRow);
        var adopted = 0; Task<bool> Adopt(CancellationToken _) { adopted++; return Task.FromResult(true); }
        var subject = service.GuestEntryFor("zoom:7", [new(1, "zoom:7", "Ada"), new(2, "capture:camera", "HD60")], (_, name) => (name + " Lovelace", "Guest"))!;
        var guestId = GraphicsPlayoutService.InstanceIdFor("guest", "zoom:7");
        // The guest row auto-plays-out after 8 s; hold that timer so the strip stays readable.
        _heldDelays[TimeSpan.FromMilliseconds(8000)] = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        // No Program subject: refused, nothing sent, authority untouched.
        var refused = await service.ApplyLowerThirdIntentAsync(true, null, authorityIsInstances: false, Adopt);
        Assert.False(refused.Accepted); Assert.Empty(Commands); Assert.Equal(0, adopted);

        // A title on Program must survive an LT in/out.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted); ClearCommands();

        var played = await service.ApplyLowerThirdIntentAsync(true, subject, authorityIsInstances: false, Adopt);
        Assert.True(played.Accepted, played.Reason);
        Assert.Equal(new[] { "graphics.instance.create", "graphics.instance.cue", "graphics.instance.trigger" }, Actions);
        var create = Bodies("graphics.instance.create").Single();
        Assert.Equal(guestId, create.GetProperty("instanceId").GetString());
        Assert.Equal("Ada Lovelace", create.GetProperty("fields").GetProperty("name").GetString());
        Assert.Equal("zoom:7", create.GetProperty("binding").GetProperty("sourceId").GetString());
        var show = Bodies("graphics.instance.trigger").Single();
        Assert.Equal(("show", "cue", guestId), (show.GetProperty("action").GetString(), show.GetProperty("target").GetString(), show.GetProperty("instanceId").GetString()));
        Assert.Equal(1, adopted);
        Assert.True(service.LowerThirdOnAir);
        Assert.Equal(new[] { "playlist-title", guestId }.OrderBy(x => x), service.Strip.OnProgram.Select(i => i.InstanceId).OrderBy(x => x));

        // Already on air: idempotent, no replay, no second authority flip.
        ClearCommands();
        Assert.True((await service.ApplyLowerThirdIntentAsync(true, subject, authorityIsInstances: true, Adopt)).Accepted);
        Assert.Empty(Commands); Assert.Equal(1, adopted);

        // LT out hides only the guest-row lower third; the title stays.
        var hidden = await service.ApplyLowerThirdIntentAsync(false, subject, authorityIsInstances: true, Adopt);
        Assert.True(hidden.Accepted, hidden.Reason);
        var hide = Assert.Single(Commands);
        Assert.Equal(("graphics.instance.trigger", "hide", guestId), (hide.Action, hide.Body.GetProperty("action").GetString(), hide.Body.GetProperty("instanceId").GetString()));
        Assert.Equal(new[] { "playlist-title" }, service.Strip.OnProgram.Select(i => i.InstanceId));
        Assert.False(service.LowerThirdOnAir);
        // LT out with nothing on air is an accepted no-op.
        ClearCommands();
        Assert.True((await service.ApplyLowerThirdIntentAsync(false, subject, authorityIsInstances: true, Adopt)).Accepted); Assert.Empty(Commands);
    }

    [Fact] public async Task LowerThirdOnAirFollowsTheStripLowerThirdChip()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        var guestId = GraphicsPlayoutService.InstanceIdFor("guest", "zoom:7");
        NativeGraphicsInstance Program(string id, string templateId) => new() { InstanceId = id, Revision = 1, ProgramRevision = 1, Visibility = "visible", TemplateId = templateId, TemplateVersion = 1 };
        service.Observe(_state = _state with { Instances = [Program("playlist-title", "centered-title"), Program("graphic-logo", "corner-logo")] });
        Assert.False(service.LowerThirdOnAir);
        service.Observe(_state = _state with { Instances = [Program("playlist-title", "centered-title"), Program(guestId, "minimal-accent")] });
        Assert.True(service.LowerThirdOnAir);
        // A Design-made lower third counts by template family, a hidden one does not.
        service.Observe(_state = _state with { Instances = [Program("graphic-design", "compact-solid")] });
        Assert.True(service.LowerThirdOnAir);
        service.Observe(_state = _state with { Instances = [Program("graphic-design", "compact-solid") with { Visibility = "building-out" }] });
        Assert.False(service.LowerThirdOnAir);
        Assert.False(GraphicsPlayoutService.IsLowerThirdOnAir(GraphicsStripState.Empty, library.Document.Playlist, _state));
    }

    // ---- Shot layouts (#1076 slice 2) --------------------------------------------------------
    [Theory]
    [InlineData(99, "crawl", "\"Final\"")]
    [InlineData(2, "different-feed", "\"Final\"")]
    [InlineData(2, "crawl", "12")]
    public async Task DataLayoutRefusesAnInvalidSecondRowBeforeAnyMutation(long revision, string source, string heading)
    {
        var (library, service, layout) = await DataLayout(revision, source, heading);
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        var takes = 0;
        var result = await service.CueAndTakeLayoutAsync(layout.Id, SelectScene, null, null,
            _ => { takes++; return Task.FromResult<string?>(null); });
        Assert.False(result.Accepted, result.Reason);
        Assert.Contains("Crawl", result.Reason);
        Assert.Empty(_sceneSelects);
        Assert.Empty(Commands);
        Assert.Equal(0, takes);
    }

    [Fact]
    public async Task DataLayoutResolvesPublishedAndLoadedRevisionsWithoutChangingPlaylist()
    {
        var (library, _, layout) = await DataLayout(2, "crawl", "\"Final\"");
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        var (reopened, service) = Build();
        await reopened.LoadAsync();
        var result = await service.CueLayoutAsync(layout.Id, SelectScene, null);
        Assert.True(result.Accepted, result.Reason);
        var fields = Bodies("graphics.instance.create").Select(body => body.GetProperty("fields")).ToArray();
        Assert.Contains("Ada", fields[0].GetProperty("table-row-1").GetString());
        Assert.Equal("Scores tonight", fields[0].GetProperty("heading").GetString());
        Assert.Equal("2", fields[1].GetProperty(GraphicsCrawlData.RevisionField).GetString());
        Assert.Equal("Final", fields[1].GetProperty("heading").GetString());
        Assert.All(reopened.Document.Playlist, row => Assert.Equal(1, row.DataBinding!.Revision));
        Assert.All(reopened.Document.Playlist, row => Assert.False(row.Fields.ContainsKey("heading")));
    }

    [Theory]
    [InlineData(GraphicsCrawlData.RevisionField, false)]
    [InlineData("heading", true)]
    public async Task DataLayoutRefusesGeneratedOrRePinnedFields(string key, bool changePin)
    {
        var (library, service, layout) = await DataLayout(2, "crawl", "\"Final\"");
        var settings = layout.RowSettings.ToDictionary();
        settings["crawl"] = settings["crawl"] with
        {
            Overrides = new Dictionary<string, JsonElement> { [key] = JsonSerializer.SerializeToElement("Final") }
        };
        Assert.True(await library.SaveShotLayoutsAsync([layout with { RowSettings = settings }]), library.Status);
        if (changePin)
        {
            var replacement = Template("design-crawl", "title", "declarative", GraphicsCrawlData.ItemsField, GraphicsCrawlData.RevisionField)
                with { TemplateVersion = 2, Definition = JsonSerializer.SerializeToElement(new { templateId = "design-crawl", templateVersion = 2 }) };
            _state = _state with { Templates = [.. _state.Templates, replacement] };
            Assert.True(await library.SavePlaylistAsync(library.Document.Playlist.Select(row =>
                row.Id == "crawl" ? row with { TemplateVersion = 2 } : row).ToArray()), library.Status);
        }

        var result = await service.CueLayoutAsync(layout.Id, SelectScene, null);
        Assert.False(result.Accepted);
        Assert.Contains(key, result.Reason);
        Assert.Empty(_sceneSelects);
        Assert.Empty(Commands);
    }

    [Fact]
    public async Task GuardedLayoutKeepsProgramGraphicsAndDoesNotRideAnUnguardedTake()
    {
        var (library, service, layout) = await DataLayout(2, "crawl", "\"Final\"");
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        _state = _state with { Instances = [new() { InstanceId = "manual-logo", Revision = 1, ProgramRevision = 1, Visibility = "visible" }] };

        var result = await service.CueLayoutAsync(layout.Id, SelectScene, null);
        Assert.True(result.Accepted, result.Reason);
        var progress = Assert.IsType<GraphicsLayoutProgress>(service.LayoutProgress);
        Assert.Equal("Ready for Take", progress.State);
        Assert.Equal(new long?[] { 1, 2 }, progress.Rows.Select(row => row.DataRevision));
        Assert.All(progress.Rows, row => Assert.Equal("Ready", row.State));
        var cues = Bodies("graphics.instance.cue");
        Assert.Equal(2, cues.Count());
        Assert.All(cues, cue => Assert.False(cue.GetProperty("rideTake").GetBoolean()));
        Assert.Single(cues.Select(cue => cue.GetProperty("cueOwner").GetString()).Distinct());
        Assert.Empty(Bodies("graphics.instance.trigger"));
        Assert.Contains(_state.Instances, instance => instance.InstanceId == "manual-logo" && instance.ProgramRevision == 1);
    }

    [Fact]
    public async Task GuardedLayoutCancellationReleasesOnlyItsOwnCues()
    {
        var (library, service, layout) = await DataLayout(2, "crawl", "\"Final\"");
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        _state = _state with { Instances = [new() { InstanceId = "independent-cue", Revision = 1, CueRevision = 1, Cued = true }] };
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _heldDelays[TimeSpan.FromMilliseconds(50)] = pending;
        _holdCueInstance = "playlist-crawl";
        var cue = service.CueLayoutAsync(layout.Id, SelectScene, null);
        await _heldCueReached.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(service.LayoutPreparing);
        Assert.Equal("Preparing", service.LayoutProgress?.State);
        Assert.True((await service.CancelLayoutAsync()).Accepted);

        Assert.False((await cue).Accepted);
        Assert.False(service.LayoutPreparing);
        Assert.Null(service.LayoutProgress);
        Assert.Equal(["independent-cue"], _state.Instances.Where(instance => instance.Cued || instance.CuePending).Select(instance => instance.InstanceId));
        Assert.All(Bodies("graphics.instance.trigger"), body =>
            Assert.Equal("uncue", body.GetProperty("action").GetString()));
    }

    [Fact]
    public async Task GuardedLayoutDetectsPreviewEditAndPreservesAReplacementCue()
    {
        var (library, service, layout) = await DataLayout(2, "crawl", "\"Final\"");
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _heldDelays[TimeSpan.FromMilliseconds(50)] = pending;
        _holdCueInstance = "playlist-crawl";
        var stamp = "preview-1";
        var cue = service.CueLayoutAsync(layout.Id, SelectScene, null, previewStamp: () => stamp);
        await _heldCueReached.Task.WaitAsync(TimeSpan.FromSeconds(5));
        stamp = "preview-2";
        _state = _state with { Instances = _state.Instances.Select(instance => instance.InstanceId switch
        {
            "playlist-scores" => instance with { CueOwner = "" },
            _ => instance with { CuePending = false }
        }).ToArray() };
        pending.SetResult();

        Assert.False((await cue).Accepted);
        Assert.Null(service.PreparedLayout);
        Assert.Equal("playlist-scores", Assert.Single(_state.Instances, instance => instance.Cued).InstanceId);
    }

    [Fact]
    public async Task GuardedLayoutCleansUpACueWhoseResponseWasLost()
    {
        var (library, service, layout) = await DataLayout(2, "crawl", "\"Final\"");
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        _lostCueReply = "playlist-crawl";

        var result = await service.CueLayoutAsync(layout.Id, SelectScene, null);
        Assert.False(result.Accepted);
        Assert.Null(service.PreparedLayout);
        Assert.DoesNotContain(_state.Instances, instance => instance.Cued || instance.CuePending);
        Assert.Equal(2, Bodies("graphics.instance.trigger").Count());
    }

    [Fact]
    public async Task GuardedLayoutTakeReusesTheReadyAttemptOnRetry()
    {
        var (library, service, layout) = await DataLayout(2, "crawl", "\"Final\"");
        Assert.True(await library.SaveShotLayoutsAsync([layout]), library.Status);
        var attempts = new List<string>();
        Task<string?> Take(CancellationToken _)
        {
            attempts.Add(Assert.IsType<GraphicsPreparedLayout>(service.PreparedLayout).AttemptId);
            return Task.FromResult(attempts.Count == 1 ? "The reply was lost." : (string?)null);
        }

        Assert.False((await service.CueAndTakeLayoutAsync(layout.Id, SelectScene, null, null, Take)).Accepted);
        Assert.True((await service.CueAndTakeLayoutAsync(layout.Id, SelectScene, null, null, Take)).Accepted);
        Assert.Equal(2, attempts.Count);
        Assert.Equal(attempts[0], attempts[1]);
        Assert.Equal(2, Bodies("graphics.instance.cue").Count());
    }

    private async Task<(GraphicsWorkspaceLibrary Library, GraphicsPlayoutService Service, ShotLayout Layout)> DataLayout(
        long revision, string source, string heading)
    {
        var scores = GraphicsLocalData.Parse("id,name,score\na,Ada,8", "csv", "id");
        var crawl = GraphicsLocalData.Parse("[{\"line\":\"First\"}]", "json", null);
        var scoreBinding = new GraphicsDataBinding("scores", "id", ["name", "score"], "score", 1, 1);
        var crawlBinding = new GraphicsDataBinding("crawl", null, ["line"], null, 8, 1) { Crawl = new() };
        var scoreFields = GraphicsDataTable.Fields(scores, scoreBinding);
        var crawlFields = GraphicsDataTable.Fields(crawl, crawlBinding);
        _state = _state with { Templates = [.. _state.Templates,
            Template("design-scores", "title", "declarative", [.. scoreFields.Keys, "heading"]),
            Template("design-crawl", "title", "declarative", [.. crawlFields.Keys, "heading"])] };
        var (library, service) = Build();
        Assert.NotNull(await library.SaveLocalDataAsync("scores", "Scores", scores));
        Assert.NotNull(await library.SaveLocalDataAsync("scores", "Scores", GraphicsLocalData.Parse("id,name,score\nb,Bob,9", "csv", "id")));
        Assert.NotNull(await library.SaveLocalDataAsync("crawl", "Crawl", crawl));
        Assert.NotNull(await library.SaveLocalDataAsync("crawl", "Crawl", GraphicsLocalData.Parse("[{\"line\":\"Second\"}]", "json", null)));
        Assert.True(await library.SavePlaylistAsync([
            new() { Id = "scores", Name = "Scores", TemplateId = "design-scores", TemplateVersion = 1, Fields = scoreFields, DataBinding = scoreBinding },
            new() { Id = "crawl", Name = "Crawl", TemplateId = "design-crawl", TemplateVersion = 1, Fields = crawlFields, DataBinding = crawlBinding }
        ]), library.Status);
        var layout = JsonSerializer.Deserialize<ShotLayout>($$"""
            {"id":"results","name":"Results","sceneId":"intro","rowIds":["scores","crawl"],"playOutOthers":true,
             "rowSettings":{"scores":{"overrides":{"heading":"Scores tonight"},"dataPin":{"sourceId":"scores","revision":1} },
                            "crawl":{"overrides":{"heading":{{heading}} },"dataPin":{"sourceId":"{{source}}","revision":{{revision}} } } } }
            """, Json)!;
        return (library, service, layout);
    }

    [Fact] public async Task DefaultShotLayoutsShipWithTheDefaultPlaylist()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.Equal(new[] { "open", "two-up", "panel", "clean" }, service.Layouts.Select(l => l.Id));
        Assert.Equal(new[] { "Shift+F1", "Shift+F2", "Shift+F3", "Shift+F11" }, service.Layouts.Select(l => l.Hotkey));
        Assert.Equal(new[] { "intro", "interview", "panel", "" }, service.Layouts.Select(l => l.SceneId));
        Assert.Equal(new[] { "title", "logo" }, service.Layout("open")!.RowIds); Assert.Equal(new[] { "guest" }, service.Layout("two-up")!.RowIds); Assert.Equal(new[] { "guest", "logo" }, service.Layout("panel")!.RowIds);
        Assert.True(service.Layout("clean")!.PlayOutOthers); Assert.Empty(service.Layout("clean")!.RowIds);
        Assert.All(service.Layouts.SelectMany(l => l.RowIds), id => Assert.NotNull(service.Row(id)));
        var loaded = await new GraphicsDocumentStore(Path.Combine(_directory, "show.json")).LoadAsync();
        Assert.Equal(4, loaded.Document.ShotLayouts.Count);
        // Layouts save validates row ids and unique hotkeys; a bad set leaves the previous one.
        Assert.False(await library.SaveShotLayoutsAsync([new() { Id = "x", Name = "X", RowIds = ["missing-row"] }])); Assert.Contains("missing-row", library.Status);
        Assert.False(await library.SaveShotLayoutsAsync([new() { Id = "a", Name = "A", Hotkey = "Shift+F4" }, new() { Id = "b", Name = "B", Hotkey = "Shift+F4" }]));
        Assert.Equal(4, service.Layouts.Count);
        Assert.True(await library.SaveShotLayoutsAsync([.. service.Layouts, new() { Id = "wide", Name = "Wide", SceneId = "speaker-slides", RowIds = ["title"], Hotkey = "Shift+F4" }]), library.Status);
        Assert.Equal(5, service.Layouts.Count); Assert.Empty(Commands);
    }

    [Fact] public async Task CueLayoutSelectsSceneThenCuesEveryRowToPreview()
    {
        var (_, service) = await BuildWithDefaultPlaylist();
        var result = await service.CueLayoutAsync("open", SelectScene, programSubject: null);
        Assert.True(result.Accepted, result.Reason);
        Assert.Equal(new[] { "intro" }, _sceneSelects);
        // Scene first, then create+cue per row; nothing is shown (the Take carries it).
        Assert.Equal(new[] { "graphics.instance.create", "graphics.instance.cue", "graphics.instance.create", "graphics.instance.cue" }, Actions);
        Assert.Equal(new[] { "playlist-title", "playlist-logo" }, Bodies("graphics.instance.cue").Select(b => b.GetProperty("instanceId").GetString()));
        Assert.All(Bodies("graphics.instance.cue"), b => Assert.True(b.GetProperty("rideTake").GetBoolean()));
        Assert.Equal(new[] { "playlist-title", "playlist-logo" }, service.Strip.OnPreview.Select(i => i.InstanceId)); Assert.Empty(service.Strip.OnProgram);
        Assert.Contains("Open cued", result.Reason);
        // A Guest-row layout cues the entries routed in that scene when given, else the Program subject.
        ClearCommands(); _sceneSelects.Clear();
        var ada = service.GuestEntryFor("zoom:7", [new(1, "zoom:7", "Ada"), new(2, "capture:camera", "HD60")], (_, n) => (n, "Guest"))!;
        var hd60 = service.GuestEntryFor("capture:camera", [new(1, "zoom:7", "Ada"), new(2, "capture:camera", "HD60")], (_, n) => (n, "Guest"))!;
        var panel = await service.CueLayoutAsync("panel", SelectScene, programSubject: ada, sceneGuests: [ada, hd60]);
        Assert.True(panel.Accepted, panel.Reason); Assert.Equal(new[] { "panel" }, _sceneSelects);
        Assert.Equal(new[] { GraphicsPlayoutService.InstanceIdFor(ada), GraphicsPlayoutService.InstanceIdFor(hd60), "playlist-logo" }, Bodies("graphics.instance.cue").Select(b => b.GetProperty("instanceId").GetString()));
        ClearCommands();
        var twoUp = await service.CueLayoutAsync("two-up", SelectScene, programSubject: ada, sceneGuests: []);
        Assert.True(twoUp.Accepted, twoUp.Reason);
        Assert.Equal(new[] { GraphicsPlayoutService.InstanceIdFor(ada) }, Bodies("graphics.instance.cue").Select(b => b.GetProperty("instanceId").GetString()));
        // No subject at all: refused before the scene changes.
        ClearCommands(); _sceneSelects.Clear();
        var refused = await service.CueLayoutAsync("two-up", SelectScene, programSubject: null);
        Assert.False(refused.Accepted); Assert.Contains("Program subject", refused.Reason); Assert.Empty(_sceneSelects); Assert.Empty(Commands);
        // Cue and Take: the Take runs after the cue; a refused Take is reported, not hidden.
        _sceneSelects.Clear(); var takes = 0;
        var taken = await service.CueAndTakeLayoutAsync("open", SelectScene, null, null, _ => { takes++; return Task.FromResult<string?>(null); });
        Assert.True(taken.Accepted, taken.Reason); Assert.Equal(1, takes); Assert.Equal(new[] { "intro" }, _sceneSelects);
        var notTaken = await service.CueAndTakeLayoutAsync("open", SelectScene, null, null, _ => Task.FromResult<string?>("Take is unavailable."));
        Assert.False(notTaken.Accepted); Assert.Contains("Take is unavailable.", notTaken.Reason);
    }

    [Fact] public async Task LayoutWithMissingSceneIsRefused()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.True(await library.SaveShotLayoutsAsync([.. service.Layouts, new() { Id = "gone", Name = "Gone", SceneId = "deleted-scene", RowIds = ["title"], Hotkey = "Shift+F4" }]), library.Status);
        var result = await service.CueLayoutAsync("gone", SelectScene, programSubject: null);
        Assert.False(result.Accepted);
        Assert.Contains("deleted-scene", result.Reason); Assert.Contains("Gone", result.Reason);
        Assert.Equal(new[] { "deleted-scene" }, _sceneSelects);
        Assert.Empty(Commands); Assert.Empty(service.Strip.OnPreview);
        // A layout naming a playlist row that no longer exists is refused before the scene is touched.
        _sceneSelects.Clear();
        Assert.True(await library.SavePlaylistAsync(library.Document.Playlist.Where(r => r.Id != "logo").ToArray()), library.Status);
        var broken = await service.CueLayoutAsync("open", SelectScene, programSubject: null);
        Assert.False(broken.Accepted); Assert.Contains("'logo'", broken.Reason); Assert.Empty(_sceneSelects); Assert.Empty(Commands);
        Assert.False((await service.CueLayoutAsync("no-such-layout", SelectScene, null)).Accepted);
    }

    [Fact] public async Task CleanLayoutPlaysOutEverythingAndChangesNoScene()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        _state = _state with { Instances = [.. _state.Instances, new() { InstanceId = "graphic-design", Revision = 1, ProgramRevision = 1, Visibility = "visible", TemplateId = "compact-solid" }] };
        service.Observe(_state); Assert.Equal(3, service.Strip.OnProgram.Count); ClearCommands();
        var result = await service.CueLayoutAsync("clean", SelectScene, programSubject: null);
        Assert.True(result.Accepted, result.Reason);
        Assert.Empty(_sceneSelects);
        Assert.All(Commands, c => { Assert.Equal("graphics.instance.trigger", c.Action); Assert.Equal("hide", c.Body.GetProperty("action").GetString()); });
        Assert.Equal(new[] { "graphic-design", "playlist-logo", "playlist-title" }, Commands.Select(c => c.Body.GetProperty("instanceId").GetString()).OrderBy(x => x));
        Assert.Empty(service.Strip.OnProgram); Assert.Empty(service.Strip.OnPreview);
        // Clean has nothing to Take; Cue and Take does not call the Take.
        var takes = 0;
        var cleaned = await service.CueAndTakeLayoutAsync("clean", SelectScene, null, null, _ => { takes++; return Task.FromResult<string?>(null); });
        Assert.True(cleaned.Accepted); Assert.Equal(0, takes); Assert.Empty(_sceneSelects);
        // PlayOutOthers on a layout with rows keeps the rows it carries: a cued row is not hidden,
        // but a Program graphic outside the layout is.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.True(await library.SaveShotLayoutsAsync([.. service.Layouts, new() { Id = "logo-only", Name = "Logo only", SceneId = "intro", RowIds = ["logo"], Hotkey = "Shift+F5", PlayOutOthers = true }]), library.Status);
        ClearCommands();
        var partial = await service.CueLayoutAsync("logo-only", SelectScene, null);
        Assert.True(partial.Accepted, partial.Reason);
        var hides = Commands.Where(c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide").ToArray();
        Assert.Equal(new[] { "playlist-title" }, hides.Select(h => h.Body.GetProperty("instanceId").GetString()));
        Assert.Equal(new[] { "playlist-logo" }, service.Strip.OnPreview.Select(i => i.InstanceId));
    }

    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }

    [Fact]
    public async Task ShowAfterSavingANewVersionAdoptsItAndTheDesignsMotion()
    {
        const string design = "design-guest-a1b2c3";
        var firstDefinition = JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest("lower-third", design, 1, "Guest"));
        var first = NativeGraphicsScratch.From(firstDefinition);
        var edited = GraphicsTemplateEditor.SetLayerMotion(first, "plate", "in", new("wipe", 0, 350, Direction: "left"));
        edited = GraphicsTemplateEditor.SetLayerMotion(edited, "name-label", "in", new("rise", 200, 300, Distance: 24));
        var secondManifest = System.Text.Json.Nodes.JsonNode.Parse(edited.Manifest)!.AsObject();
        secondManifest["templateVersion"] = 2;
        var secondDefinition = JsonSerializer.SerializeToElement(secondManifest);
        var second = NativeGraphicsScratch.From(secondDefinition);
        var template = Template(design, "lower-third", "declarative", "name", "title") with
            { Definition = firstDefinition, ManifestHash = first.Hash, Animations = GraphicsTemplateEditor.GraphicMotion(first) };
        _state = _state with { Templates = [.. _state.Templates, template, template with { TemplateVersion = 2,
            Definition = secondDefinition, ManifestHash = second.Hash, Animations = GraphicsTemplateEditor.GraphicMotion(second) }] };
        var (library, service) = Build();
        var row = new GraphicsPlaylistRow { Id = "guest-motion", Kind = GraphicsPlaylistRow.Guest, Name = "Guest",
            TemplateId = design, TemplateVersion = 1, Hotkey = "F6",
            Fields = new Dictionary<string, JsonElement> { ["name"] = JsonSerializer.SerializeToElement("Ada"),
                ["title"] = JsonSerializer.SerializeToElement("Engineer") } };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(row))).Accepted);
        ClearCommands();
        Assert.True(await library.SavePlaylistAsync([row with { TemplateVersion = 2 }]), library.Status);
        Assert.Null(library.Document.Playlist.Single().Motion);
        Assert.Contains(library.Document.Templates, pin => pin.TemplateId == design && pin.TemplateVersion == 2);
        var shown = await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist.Single()));
        Assert.True(shown.Accepted, shown.Reason);
        var update = Assert.Single(Bodies("graphics.instance.update"));
        Assert.Equal(2, update.GetProperty("templateVersion").GetInt32());
        Assert.Equal("cue", update.GetProperty("target").GetString());
        Assert.Equal("own", update.GetProperty("animations").GetProperty("layers").GetString());
        Assert.Equal(250, update.GetProperty("animations").GetProperty("out").GetProperty("durationMs").GetDouble());
        Assert.Equal(new[] { "graphics.instance.update", "graphics.instance.cue", "graphics.instance.trigger" }, Actions);
    }

    [Fact]
    public async Task RowMotionMakesTheWholeGraphicMoveAsOne()
    {
        const string design = "design-guest-a1b2c3";
        _state = _state with { Templates = [.. _state.Templates, Template(design, "lower-third", "declarative", "name")] };
        var (library, service) = Build();
        var row = new GraphicsPlaylistRow { Id = "guest-motion", Kind = GraphicsPlaylistRow.Guest, Name = "Guest",
            TemplateId = design, TemplateVersion = 1, Motion = GraphicsBuilder.MotionFor("slide-left", 300) };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        var shown = await service.PlayInAsync(GraphicsPlayoutService.EntryFor(row));
        Assert.True(shown.Accepted, shown.Reason);
        var animations = Assert.Single(Bodies("graphics.instance.create")).GetProperty("animations");
        Assert.Equal("whole", animations.GetProperty("layers").GetString());
        Assert.Equal("slide", animations.GetProperty("in").GetProperty("type").GetString());
    }

    [Fact]
    public async Task BuiltInRowsKeepTheDefaultFadeAndAnUnchangedRowSkipsTheUpdate()
    {
        const string id = "corporate-lower-third";
        _state = _state with { Templates = [.. _state.Templates, Template(id, "lower-third", "declarative", "name", "title")] };
        var (library, service) = Build();
        var row = new GraphicsPlaylistRow { Id = "guest-motion", Kind = GraphicsPlaylistRow.Guest, Name = "Guest",
            TemplateId = id, TemplateVersion = 1 };
        Assert.True(await library.SavePlaylistAsync([row]), library.Status);
        var entry = GraphicsPlayoutService.EntryFor(row);
        Assert.True((await service.PlayInAsync(entry)).Accepted);
        var animations = Assert.Single(Bodies("graphics.instance.create")).GetProperty("animations");
        Assert.Equal("own", animations.GetProperty("layers").GetString());
        Assert.Equal(250, animations.GetProperty("in").GetProperty("durationMs").GetDouble());
        Assert.Equal(200, animations.GetProperty("out").GetProperty("durationMs").GetDouble());
        ClearCommands();
        Assert.True((await service.PlayInAsync(entry)).Accepted);
        Assert.Empty(Bodies("graphics.instance.update"));
    }
}
