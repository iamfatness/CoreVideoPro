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
        var input = JsonDocument.Parse(body).RootElement.Clone(); _commands.Add((action, input));
        var id = input.TryGetProperty("instanceId", out var rawId) ? rawId.GetString()! : "";
        long revision = 1;
        NativeGraphicsInstance[] Map(Func<NativeGraphicsInstance, NativeGraphicsInstance> change) => _state.Instances.Select(i => i.InstanceId == id ? change(i) : i).ToArray();
        switch (action) {
            case "graphics.instance.create":
                var binding = input.TryGetProperty("binding", out var b) && b.ValueKind == JsonValueKind.Object ? JsonSerializer.Deserialize<SourceInstanceIdentity>(b, Json) : null;
                _state = _state with { Instances = [.. _state.Instances, new() { InstanceId = id, Revision = 1, TemplateId = input.GetProperty("templateId").GetString()!, TemplateVersion = 1, Scope = "global",
                    Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(input.GetProperty("fields"), Json)!, Binding = binding, SourceId = binding?.SourceId ?? "" }] };
                break;
            case "graphics.instance.update":
                revision = _state.Instances.Single(i => i.InstanceId == id).Revision + 1;
                _state = _state with { Instances = Map(i => i with { Revision = revision, Fields = JsonSerializer.Deserialize<NativeGraphicsFields>(input.GetProperty("fields"), Json)! }) };
                break;
            case "graphics.instance.cue":
                var expected = input.GetProperty("expectedRevision").GetInt64();
                _state = _state with { Instances = Map(i => i with { CueRevision = expected, CuePending = _cuePendingPolls > 0, Cued = true }) };
                break;
            case "graphics.instance.trigger":
                _state = input.GetProperty("action").GetString() switch {
                    "show" => _showDoesNotLand ? _state : _state with { Instances = Map(i => i with { ProgramRevision = i.CueRevision, Visibility = "visible", CueRevision = 0, Cued = false }) },
                    "hide" => _state with { Instances = Map(i => i with { ProgramRevision = 0, Visibility = "hidden" }) },
                    "uncue" => _state with { Instances = Map(i => i with { CueRevision = 0, Cued = false }) },
                    _ => _state };
                break;
            case "graphics.instance.remove": _state = _state with { Instances = _state.Instances.Where(i => i.InstanceId != id).ToArray() }; break;
        }
        return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = id, CurrentRevision = revision, Reason = "accepted" });
    }
    private IEnumerable<string> Actions => _commands.Select(c => c.Action);
    private IEnumerable<JsonElement> Bodies(string action) => _commands.Where(c => c.Action == action).Select(c => c.Body);

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
        Assert.Empty(_commands);
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
        _commands.Clear();
        // A row without its own motion still gets the playout default.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        Assert.Equal("fade", Bodies("graphics.instance.create").Single().GetProperty("animations").GetProperty("in").GetProperty("type").GetString());
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
        _commands.Clear();
        Assert.True((await service.ToPreviewAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        Assert.Equal(new[] { "graphics.instance.create", "graphics.instance.cue" }, Actions);
        Assert.Equal(new[] { "playlist-logo" }, service.Strip.OnPreview.Select(i => i.InstanceId));
    }

    [Fact] public async Task PlayOutHidesExactlyTheTargetInstance()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        Assert.Equal(2, service.Strip.OnProgram.Count); _commands.Clear();
        var result = await service.PlayOutAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]));
        Assert.True(result.Accepted, result.Reason);
        var hide = Assert.Single(_commands);
        Assert.Equal("graphics.instance.trigger", hide.Action); Assert.Equal("hide", hide.Body.GetProperty("action").GetString());
        Assert.Equal("playlist-logo", hide.Body.GetProperty("instanceId").GetString()); Assert.False(hide.Body.TryGetProperty("target", out _));
        Assert.Equal(new[] { "playlist-title" }, service.Strip.OnProgram.Select(i => i.InstanceId));
        Assert.False((await service.PlayOutAsync("playlist-logo")).Accepted); Assert.Single(_commands);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted); _commands.Clear();
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
        var uncue = Assert.Single(_commands);
        Assert.Equal(("graphics.instance.trigger", "uncue", "graphic-design"), (uncue.Action, uncue.Body.GetProperty("action").GetString(), uncue.Body.GetProperty("instanceId").GetString()));
        Assert.Equal(new[] { "playlist-title", guestId }, service.Strip.OnPreview.Select(i => i.InstanceId));
        _commands.Clear();
        Assert.True((await service.ClearPreviewAsync()).Accepted);
        Assert.Equal(2, _commands.Count); Assert.All(_commands, c => Assert.Equal("uncue", c.Body.GetProperty("action").GetString()));
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
        Assert.DoesNotContain(_commands, c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide");
        Assert.True(Assert.Single(service.Strip.OnProgram).AutoOutArmed);
        held.SetResult();
        var deadline = DateTime.UtcNow.AddSeconds(5);
        while (_state.Instances.Single(i => i.InstanceId == instanceId).ProgramRevision > 0 && DateTime.UtcNow < deadline) await Task.Delay(5);
        var hides = _commands.Where(c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide").ToArray();
        Assert.Equal(instanceId, Assert.Single(hides).Body.GetProperty("instanceId").GetString());
        Assert.Empty(service.Strip.OnProgram);
        held.TrySetResult(); service.Observe(_state); await Task.Delay(20);
        Assert.Single(_commands, c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide");
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
        Assert.False(refused.Accepted); Assert.Empty(_commands); Assert.Equal(0, adopted);

        // A title on Program must survive an LT in/out.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted); _commands.Clear();

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
        _commands.Clear();
        Assert.True((await service.ApplyLowerThirdIntentAsync(true, subject, authorityIsInstances: true, Adopt)).Accepted);
        Assert.Empty(_commands); Assert.Equal(1, adopted);

        // LT out hides only the guest-row lower third; the title stays.
        var hidden = await service.ApplyLowerThirdIntentAsync(false, subject, authorityIsInstances: true, Adopt);
        Assert.True(hidden.Accepted, hidden.Reason);
        var hide = Assert.Single(_commands);
        Assert.Equal(("graphics.instance.trigger", "hide", guestId), (hide.Action, hide.Body.GetProperty("action").GetString(), hide.Body.GetProperty("instanceId").GetString()));
        Assert.Equal(new[] { "playlist-title" }, service.Strip.OnProgram.Select(i => i.InstanceId));
        Assert.False(service.LowerThirdOnAir);
        // LT out with nothing on air is an accepted no-op.
        _commands.Clear();
        Assert.True((await service.ApplyLowerThirdIntentAsync(false, subject, authorityIsInstances: true, Adopt)).Accepted); Assert.Empty(_commands);
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
        Assert.Equal(5, service.Layouts.Count); Assert.Empty(_commands);
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
        _commands.Clear(); _sceneSelects.Clear();
        var ada = service.GuestEntryFor("zoom:7", [new(1, "zoom:7", "Ada"), new(2, "capture:camera", "HD60")], (_, n) => (n, "Guest"))!;
        var hd60 = service.GuestEntryFor("capture:camera", [new(1, "zoom:7", "Ada"), new(2, "capture:camera", "HD60")], (_, n) => (n, "Guest"))!;
        var panel = await service.CueLayoutAsync("panel", SelectScene, programSubject: ada, sceneGuests: [ada, hd60]);
        Assert.True(panel.Accepted, panel.Reason); Assert.Equal(new[] { "panel" }, _sceneSelects);
        Assert.Equal(new[] { GraphicsPlayoutService.InstanceIdFor(ada), GraphicsPlayoutService.InstanceIdFor(hd60), "playlist-logo" }, Bodies("graphics.instance.cue").Select(b => b.GetProperty("instanceId").GetString()));
        _commands.Clear();
        var twoUp = await service.CueLayoutAsync("two-up", SelectScene, programSubject: ada, sceneGuests: []);
        Assert.True(twoUp.Accepted, twoUp.Reason);
        Assert.Equal(new[] { GraphicsPlayoutService.InstanceIdFor(ada) }, Bodies("graphics.instance.cue").Select(b => b.GetProperty("instanceId").GetString()));
        // No subject at all: refused before the scene changes.
        _commands.Clear(); _sceneSelects.Clear();
        var refused = await service.CueLayoutAsync("two-up", SelectScene, programSubject: null);
        Assert.False(refused.Accepted); Assert.Contains("Program subject", refused.Reason); Assert.Empty(_sceneSelects); Assert.Empty(_commands);
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
        Assert.Empty(_commands); Assert.Empty(service.Strip.OnPreview);
        // A layout naming a playlist row that no longer exists is refused before the scene is touched.
        _sceneSelects.Clear();
        Assert.True(await library.SavePlaylistAsync(library.Document.Playlist.Where(r => r.Id != "logo").ToArray()), library.Status);
        var broken = await service.CueLayoutAsync("open", SelectScene, programSubject: null);
        Assert.False(broken.Accepted); Assert.Contains("'logo'", broken.Reason); Assert.Empty(_sceneSelects); Assert.Empty(_commands);
        Assert.False((await service.CueLayoutAsync("no-such-layout", SelectScene, null)).Accepted);
    }

    [Fact] public async Task CleanLayoutPlaysOutEverythingAndChangesNoScene()
    {
        var (library, service) = await BuildWithDefaultPlaylist();
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[2]))).Accepted);
        _state = _state with { Instances = [.. _state.Instances, new() { InstanceId = "graphic-design", Revision = 1, ProgramRevision = 1, Visibility = "visible", TemplateId = "compact-solid" }] };
        service.Observe(_state); Assert.Equal(3, service.Strip.OnProgram.Count); _commands.Clear();
        var result = await service.CueLayoutAsync("clean", SelectScene, programSubject: null);
        Assert.True(result.Accepted, result.Reason);
        Assert.Empty(_sceneSelects);
        Assert.All(_commands, c => { Assert.Equal("graphics.instance.trigger", c.Action); Assert.Equal("hide", c.Body.GetProperty("action").GetString()); });
        Assert.Equal(new[] { "graphic-design", "playlist-logo", "playlist-title" }, _commands.Select(c => c.Body.GetProperty("instanceId").GetString()).OrderBy(x => x));
        Assert.Empty(service.Strip.OnProgram); Assert.Empty(service.Strip.OnPreview);
        // Clean has nothing to Take; Cue and Take does not call the Take.
        var takes = 0;
        var cleaned = await service.CueAndTakeLayoutAsync("clean", SelectScene, null, null, _ => { takes++; return Task.FromResult<string?>(null); });
        Assert.True(cleaned.Accepted); Assert.Equal(0, takes); Assert.Empty(_sceneSelects);
        // PlayOutOthers on a layout with rows keeps the rows it carries: a cued row is not hidden,
        // but a Program graphic outside the layout is.
        Assert.True((await service.PlayInAsync(GraphicsPlayoutService.EntryFor(library.Document.Playlist[1]))).Accepted);
        Assert.True(await library.SaveShotLayoutsAsync([.. service.Layouts, new() { Id = "logo-only", Name = "Logo only", SceneId = "intro", RowIds = ["logo"], Hotkey = "Shift+F5", PlayOutOthers = true }]), library.Status);
        _commands.Clear();
        var partial = await service.CueLayoutAsync("logo-only", SelectScene, null);
        Assert.True(partial.Accepted, partial.Reason);
        var hides = _commands.Where(c => c.Action == "graphics.instance.trigger" && c.Body.GetProperty("action").GetString() == "hide").ToArray();
        Assert.Equal(new[] { "playlist-title" }, hides.Select(h => h.Body.GetProperty("instanceId").GetString()));
        Assert.Equal(new[] { "playlist-logo" }, service.Strip.OnPreview.Select(i => i.InstanceId));
    }

    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
