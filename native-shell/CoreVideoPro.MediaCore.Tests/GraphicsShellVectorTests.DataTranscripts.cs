using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>A scripted native core for transcripts (plan "Conformance machinery", rule 9): it
/// applies what the real core would for the commands playout sends (create, update on cue or
/// Program, cue, show/hide/uncue, crawl pause/resume/restart, clock control) and records each
/// command as the wire command the gateway builds plus the graphics node after it. Scripts:
/// "accept-all", "never-adopt" (a Program update is accepted but never adopted),
/// "adopt-after-3-polls" (Program adopts after the third 50 ms wait, as a real core prepares first),
/// "fail-preparation" (the Program update's preparation fails at the first wait), "fail-second-cue"
/// (the second cue's preparation fails). Every wait after a
/// command (for the two preparing scripts) appends the node it leaves to that step's "pollsAfter", so a
/// replay serves them in order; otherwise the node after the command stands for every wait.</summary>
internal sealed class TranscriptCore
{
    private static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);
    public NativeGraphicsState State;
    public readonly JsonArray Steps = new();
    public string Script = "accept-all";
    public DateTime Clock = new(2026, 11, 22, 12, 0, 0, DateTimeKind.Utc);

    public TranscriptCore(NativeGraphicsState initial) => State = initial;

    private int _polls;
    private int _cues;

    /// A step that is not a core command (the Shot Layout's scene select), recorded in order.
    public void Pseudo(string type, JsonObject body) =>
        Steps.Add(new JsonObject { ["command"] = new JsonObject { ["type"] = type, ["graphics"] = body }, ["nodeAfter"] = JsonSerializer.SerializeToNode(State, Web) });
    private (string Id, long Revision, NativeGraphicsFields Fields)? _pendingAdoption;

    public Task Delay(TimeSpan span, CancellationToken _)
    {
        Clock += span;
        if (_pendingAdoption is { } pending)
        {
            _polls++;
            if (Script == "adopt-after-3-polls" && _polls == 3)
            {
                State = State with { Instances = State.Instances.Select(i => i.InstanceId == pending.Id
                    ? i with { ProgramRevision = pending.Revision, ProgramFields = pending.Fields, Preparation = "ready" } : i).ToArray() };
                _pendingAdoption = null;
            }
            else if (Script == "fail-preparation" && _polls == 1)
            {
                State = State with { Instances = State.Instances.Select(i => i.InstanceId == pending.Id
                    ? i with { Preparation = "failed", Warning = "Font missing" } : i).ToArray() };
                _pendingAdoption = null;
            }
        }
        // Only a state that CHANGED during the waits is worth serving; an unchanged poll is the previous node.
        if (Steps.Count > 0 && Script is "adopt-after-3-polls" or "fail-preparation")
        {
            var step = Steps[^1]!.AsObject();
            if (step["pollsAfter"] is not JsonArray polls) step["pollsAfter"] = polls = new JsonArray();
            polls.Add(JsonSerializer.SerializeToNode(State, Web));
        }
        return Task.CompletedTask;
    }

    public Task<NativeGraphicsOperationResult> Invoke(string action, string body, CancellationToken cancellation)
    {
        Assert.True(GraphicsCommandRequest.TryBuild(action, body, State, out var command, out _, out var error), action + ": " + error);
        var graphics = JsonNode.Parse(command!.ExtensionData!["graphics"].GetRawText())!.AsObject();
        foreach (var key in new[] { "requestId", "clientId", "schemaVersion", "coreEpoch" }) graphics.Remove(key);
        var input = JsonDocument.Parse(body).RootElement;
        var id = input.TryGetProperty("instanceId", out var rawId) ? rawId.GetString()! : "";
        NativeGraphicsInstance[] Map(Func<NativeGraphicsInstance, NativeGraphicsInstance> change) => State.Instances.Select(i => i.InstanceId == id ? change(i) : i).ToArray();
        long Next() => State.Instances.Single(i => i.InstanceId == id).Revision + 1;
        NativeGraphicsFields Fields(JsonElement element) => JsonSerializer.Deserialize<NativeGraphicsFields>(element, Web)!;
        switch (action)
        {
            case "graphics.instance.create":
                State = State with { Instances = [.. State.Instances, new() { InstanceId = id, Revision = 1, TemplateId = input.GetProperty("templateId").GetString()!,
                    TemplateVersion = input.GetProperty("templateVersion").GetInt32(), Scope = "global", Fields = Fields(input.GetProperty("fields")) }] };
                break;
            case "graphics.instance.update":
            {
                var revision = Next(); var fields = Fields(input.GetProperty("fields"));
                var program = input.GetProperty("target").GetString() == "program";
                State = State with { Instances = Map(i => {
                    var updated = i with { Revision = revision, Fields = fields };
                    if (!program || Script == "never-adopt") return updated;
                    if (Script is "adopt-after-3-polls" or "fail-preparation") { _pendingAdoption = (id, revision, fields); return updated with { Preparation = "preparing" }; }
                    // A playing loop crawl adopts at its loop boundary: the update is pending until then.
                    if (i.CrawlPlayback.Length > 0) return updated with { CrawlPendingRevision = revision };
                    return updated with { ProgramRevision = revision, ProgramFields = fields };
                }) };
                break;
            }
            case "graphics.instance.cue":
            {
                _cues++;
                var owner = input.TryGetProperty("cueOwner", out var ownerValue) ? ownerValue.GetString()! : "";
                var failed = Script == "fail-second-cue" && _cues == 2;
                State = State with { Instances = Map(i => i with { CueRevision = input.GetProperty("expectedRevision").GetInt64(), CueTemplateVersion = i.TemplateVersion,
                    Cued = true, CueOwner = owner, CuePreparation = failed ? "failed" : "ready", CueWarning = failed ? "Font missing" : "" }) };
                break;
            }
            case "graphics.instance.trigger":
                State = State with { Instances = Map(i => input.GetProperty("action").GetString() switch {
                    "show" => i with { ProgramRevision = i.CueRevision, ProgramTemplateVersion = i.TemplateVersion, ProgramFields = i.Fields, Visibility = "on-air",
                        CueRevision = 0, Cued = false, CrawlPlayback = i.TemplateId.StartsWith("design-headlines", StringComparison.Ordinal) ? "playing" : "" },
                    "hide" => i with { ProgramRevision = 0, Visibility = "hidden", CrawlPlayback = "" },
                    "uncue" => input.TryGetProperty("cueOwner", out var owner) && owner.GetString() != i.CueOwner ? i
                        : i with { CueRevision = 0, Cued = false, CueOwner = "", CuePreparation = "unprepared" },
                    "pause" => i with { CrawlPlayback = "paused" },
                    "resume" or "restart" => i with { CrawlPlayback = "playing" },
                    _ => i }) };
                break;
        }
        var current = State.Instances.FirstOrDefault(i => i.InstanceId == id);
        Steps.Add(new JsonObject { ["command"] = new JsonObject { ["type"] = command.Type, ["graphics"] = graphics },
            ["nodeAfter"] = JsonSerializer.SerializeToNode(State, Web) });
        return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = id, CurrentRevision = current?.Revision ?? 0, Reason = "accepted" });
    }
}

/// <summary>G4-G7 Tasks 10-11: data publishing and crawl transcripts recorded from the REAL
/// GraphicsPlayoutService (StageDataAsync, PublishDataAsync, ControlCrawlAsync,
/// SetDataLiveFollowAsync, PlayInAsync) over documents/windows-data.json.</summary>
public sealed partial class GraphicsShellVectorTests
{
    /// <summary>The design templates of windows-data.json as the core lists them once installed.</summary>
    internal static NativeGraphicsTemplate[] WindowsDataTemplates() =>
        WindowsDataShow().Designs.Select(design => (JsonSerializer.Deserialize<NativeGraphicsTemplate>(design.Definition, Web)! with
            { Runtime = "declarative", ManifestHash = new string('f', 64) })).ToArray();

    internal static NativeGraphicsInstance OnProgram(string rowId, string templateId, long revision = 3, int programTemplateVersion = 1, string crawl = "") => new()
    {
        InstanceId = GraphicsPlayoutService.InstanceIdFor(rowId, ""), Revision = revision, ProgramRevision = revision, TemplateId = templateId,
        TemplateVersion = 1, ProgramTemplateVersion = programTemplateVersion, Scope = "global", Visibility = "on-air", CrawlPlayback = crawl,
    };

    private static JsonObject RunTranscript(string id, NativeGraphicsInstance[] instances, JsonObject intent, string script,
        Func<GraphicsPlayoutService, GraphicsWorkspaceLibrary, Task<NativeGraphicsOperationResult>> run)
    {
        var directory = Path.Combine(Path.GetTempPath(), "cvp-data-transcript-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var initial = new NativeGraphicsState { SchemaVersion = 1, CoreEpoch = "epoch-1", Templates = WindowsDataTemplates(), Instances = instances };
            var core = new TranscriptCore(initial) { Script = script };
            var path = Path.Combine(directory, "show.json");
            File.Copy(DocumentPath("windows-data.json"), path);
            var session = new GraphicsWorkspaceSession(core.Invoke, () => core.State); session.Observe(initial);
            var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(path), session);
            library.LoadAsync().GetAwaiter().GetResult();
            if (intent["crawlMode"] is JsonValue mode)
            {
                var target = library.Document.Playlist.Single(r => r.Id == (string)intent["rowId"]!);
                var patched = target with { DataBinding = target.DataBinding! with { Crawl = target.DataBinding.Crawl! with { Mode = mode.GetValue<string>() } } };
                Assert.True(library.SavePlaylistAsync(GraphicsBuilder.Upsert(library.Document.Playlist, patched)).GetAwaiter().GetResult());
            }
            var service = new GraphicsPlayoutService(library, core.Invoke, () => core.State, core.Delay, () => core.Clock);
            var result = run(service, library).GetAwaiter().GetResult();
            var rowId = (string)intent["rowId"]!;
            var row = library.Document.Playlist.FirstOrDefault(r => r.Id == rowId);
            var expected = new JsonObject
            {
                ["steps"] = core.Steps, ["accepted"] = result.Accepted, ["reason"] = result.Reason,
                ["documentAfter"] = new JsonObject
                {
                    ["row"] = row is null ? null : new JsonObject { ["fields"] = JsonSerializer.SerializeToNode(row.Fields, Web), ["dataBinding"] = JsonSerializer.SerializeToNode(row.DataBinding, Web) },
                    ["dataSources"] = new JsonArray(library.Document.DataSources.Select(s => (JsonNode)new JsonObject {
                        ["id"] = s.Id, ["name"] = s.Name, ["revision"] = s.Revision, ["data"] = JsonSerializer.SerializeToNode(s.Data, Web) }).ToArray()),
                },
            };
            return (JsonObject)Row(id, "dataTranscript", new JsonObject
            {
                ["document"] = "documents/windows-data.json", ["initialNode"] = JsonSerializer.SerializeToNode(initial, Web), ["intent"] = intent, ["script"] = script,
            }, expected);
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public void DataTranscripts()
    {
        var rows = new JsonArray();
        var table = WindowsDataShow().Playlist[0]; var crawl = WindowsDataShow().Playlist[1];
        JsonObject Intent(string op, string rowId, string? file = null, string? action = null, bool? enabled = null)
        {
            var o = new JsonObject { ["op"] = op, ["rowId"] = rowId };
            if (file is not null) o["dataFile"] = file;
            if (action is not null) o["action"] = action;
            if (enabled is not null) o["enabled"] = enabled;
            return o;
        }
        GraphicsLocalData Read(string file, GraphicsPlaylistRow row) => GraphicsLocalData.ReadAsync(DataPath(file), row.DataBinding!.KeyColumn).GetAwaiter().GetResult();
        // The control passes the row's own binding object (record equality compares the Columns array by reference).
        static GraphicsDataBinding Bound(GraphicsWorkspaceLibrary library, string rowId) => library.Document.Playlist.Single(r => r.Id == rowId).DataBinding!;
        var tableLive = OnProgram("leaderboard", table.TemplateId);

        // Task 10: publish and stage a table.
        rows.Add(RunTranscript("publish-table-not-on-air", [], Intent("publish", "leaderboard"), "accept-all", (s, _) => s.PublishDataAsync("leaderboard")));
        rows.Add(RunTranscript("publish-table-on-program", [tableLive], Intent("publish", "leaderboard"), "accept-all", (s, _) => s.PublishDataAsync("leaderboard")));
        rows.Add(RunTranscript("publish-table-older-program-version", [tableLive with { ProgramTemplateVersion = 0, TemplateVersion = 1 }],
            Intent("publish", "leaderboard"), "accept-all", (s, _) => s.PublishDataAsync("leaderboard")));
        rows.Add(RunTranscript("publish-table-never-adopted", [tableLive], Intent("publish", "leaderboard"), "never-adopt", (s, _) => s.PublishDataAsync("leaderboard")));
        rows.Add(RunTranscript("stage-table-manual", [tableLive], Intent("stage", "leaderboard", "data/scores-v2.csv"), "accept-all",
            (s, l) => s.StageDataAsync("leaderboard", Bound(l, "leaderboard"), Read("data/scores-v2.csv", table))));
        rows.Add(RunTranscript("stage-table-columns-no-longer-fit", [tableLive], Intent("stage", "leaderboard", "data/scores-no-points.csv"), "accept-all",
            (s, l) => s.StageDataAsync("leaderboard", Bound(l, "leaderboard"), Read("data/scores-no-points.csv", table))));
        rows.Add(RunTranscript("publish-table-adopted-after-polls", [tableLive], Intent("publish", "leaderboard"), "adopt-after-3-polls", (s, _) => s.PublishDataAsync("leaderboard")));
        rows.Add(RunTranscript("publish-table-preparation-fails", [tableLive], Intent("publish", "leaderboard"), "fail-preparation", (s, _) => s.PublishDataAsync("leaderboard")));

        // Task 11: crawls.
        var crawlLive = OnProgram("headlines", crawl.TemplateId, crawl: "playing");
        rows.Add(RunTranscript("show-crawl", [], Intent("show", "headlines"), "accept-all", (s, _) => s.PlayInAsync(GraphicsPlayoutService.EntryFor(crawl))));
        foreach (var (action, playback) in new[] { ("pause", "playing"), ("resume", "paused"), ("restart", "playing") })
            rows.Add(RunTranscript("crawl-" + action + "-on-program", [crawlLive with { CrawlPlayback = playback }], Intent("crawl", "headlines", action: action), "accept-all",
                (s, _) => s.ControlCrawlAsync("headlines", action)));
        rows.Add(RunTranscript("crawl-pause-not-on-program", [], Intent("crawl", "headlines", action: "pause"), "accept-all", (s, _) => s.ControlCrawlAsync("headlines", "pause")));
        rows.Add(RunTranscript("crawl-bad-action", [crawlLive], Intent("crawl", "headlines", action: "stop"), "accept-all", (s, _) => s.ControlCrawlAsync("headlines", "stop")));
        rows.Add(RunTranscript("publish-crawl-loop-playing", [crawlLive], Intent("publish", "headlines"), "accept-all", (s, _) => s.PublishDataAsync("headlines")));
        var single = Intent("publish", "headlines"); single["crawlMode"] = "single";
        rows.Add(RunTranscript("publish-crawl-single-pass", [crawlLive], single, "accept-all", (s, _) => s.PublishDataAsync("headlines")));
        rows.Add(RunTranscript("crawl-pause-while-exiting", [crawlLive with { Visibility = "exiting" }], Intent("crawl", "headlines", action: "pause"), "accept-all",
            (s, _) => s.ControlCrawlAsync("headlines", "pause")));
        rows.Add(RunTranscript("crawl-pause-on-a-table-without-playback", [tableLive], Intent("crawl", "leaderboard", action: "pause"), "accept-all",
            (s, _) => s.ControlCrawlAsync("leaderboard", "pause")));
        rows.Add(RunTranscript("stage-crawl-live-follow", [crawlLive], Intent("stage", "headlines", "data/crawl-v2.csv"), "accept-all",
            (s, l) => s.StageDataAsync("headlines", Bound(l, "headlines"), Read("data/crawl-v2.csv", crawl))));
        rows.Add(RunTranscript("live-follow-on-a-table-row", [], Intent("liveFollow", "leaderboard", enabled: true), "accept-all", (s, _) => s.SetDataLiveFollowAsync("leaderboard", true)));
        rows.Add(RunTranscript("live-follow-off-on-a-crawl", [], Intent("liveFollow", "headlines", enabled: false), "accept-all", (s, _) => s.SetDataLiveFollowAsync("headlines", false)));
        Check("data-transcripts", "Data publishing and crawl control transcripts from the real GraphicsPlayoutService over documents/windows-data.json (wire command + node after each step, reason, the row and datasets after).", rows);
    }
}
