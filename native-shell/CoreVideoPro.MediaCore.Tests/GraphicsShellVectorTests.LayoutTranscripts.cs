using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 14: the layout resolver (GraphicsLayoutResolver.Resolve: data pin, then
/// overrides) and Shot Layout cue transcripts recorded from the REAL GraphicsPlayoutService
/// (CueLayoutAsync legacy and guarded paths, CancelLayoutAsync) over documents/windows-layouts.json.
/// Attempt ids ("layout-" + a GUID) are written as ATTEMPT.</summary>
public sealed partial class GraphicsShellVectorTests
{
    private static readonly SourceInstanceIdentity Guest100 = new() { SourceId = "zoom:100", InstanceId = "zoom:100-gen-1", ProcessEpoch = "zoom-a", Generation = 1 };

    private static NativeGraphicsState LayoutState(params NativeGraphicsInstance[] instances) => new()
    {
        SchemaVersion = 1, CoreEpoch = "epoch-1",
        Templates = RealCatalog().Templates.Concat(WindowsDataTemplates()).ToArray(), Instances = instances,
        Sources = [new() { VideoOn = true, DisplayName = "Ada", Identity = Guest100 }],
    };

    private static JsonObject Resolve(GraphicsShowDocument document, NativeGraphicsState state, string layoutId, GraphicsPlaylistEntry entry)
    {
        try
        {
            var layout = document.ShotLayouts.Single(l => l.Id == layoutId);
            var resolved = GraphicsLayoutResolver.Resolve(document, state, layout, entry);
            return new JsonObject
            {
                ["fields"] = JsonSerializer.SerializeToNode(resolved.Row.Fields, Web), ["identity"] = JsonSerializer.SerializeToNode(resolved.Identity, Web),
                ["dataRevision"] = resolved.DataRevision, ["dataReceivedAt"] = resolved.DataReceivedAt?.ToString("O"),
            };
        }
        catch (InvalidDataException ex) { return new JsonObject { ["error"] = ex.Message }; }
    }

    private static JsonObject WithoutTemplates(NativeGraphicsState state)
    {
        var node = JsonSerializer.SerializeToNode(state, Web)!.AsObject();
        node.Remove("templates");
        return node;
    }

    [Fact]
    public void LayoutTranscripts()
    {
        var rows = new JsonArray();
        var show = WindowsLayoutsShow();
        var state = LayoutState();
        GraphicsPlaylistEntry Entry(string rowId) => GraphicsPlayoutService.EntryFor(show.Playlist.Single(r => r.Id == rowId));
        ShotLayout With(string id, ShotLayoutRowSettings settings, string rowId) => show.ShotLayouts.Single(l => l.Id == id) with
            { RowSettings = new Dictionary<string, ShotLayoutRowSettings>(StringComparer.Ordinal) { [rowId] = settings } };
        var guest = new GraphicsPlaylistEntry("guest", "zoom:100", "Ada", "Host") { SlotNumber = 1 };
        var pinHash = new GraphicsSavedTemplate("centered-title", 1, new string('0', 64), JsonSerializer.SerializeToElement(new { templateId = "centered-title", templateVersion = 1 }));
        var cases = new (string Id, GraphicsShowDocument Document, NativeGraphicsState State, string Layout, GraphicsPlaylistEntry Entry, JsonObject? Patch)[]
        {
            ("revision-2-pin-materialized", show, state, "final-scores", Entry("leaderboard"), null),
            ("override-applied", show, state, "final-scores", Entry("title"), null),
            ("published-pin", show with { ShotLayouts = [With("final-scores", new() { DataPin = new("scores", 1) }, "leaderboard")] }, state, "final-scores", Entry("leaderboard"),
                new JsonObject { ["layoutRowSettings"] = new JsonObject { ["leaderboard"] = new JsonObject { ["dataPin"] = new JsonObject { ["sourceId"] = "scores", ["revision"] = 1 } } } }),
            ("revision-unavailable", show with { ShotLayouts = [With("final-scores", new() { DataPin = new("scores", 5) }, "leaderboard")] }, state, "final-scores", Entry("leaderboard"),
                new JsonObject { ["layoutRowSettings"] = new JsonObject { ["leaderboard"] = new JsonObject { ["dataPin"] = new JsonObject { ["sourceId"] = "scores", ["revision"] = 5 } } } }),
            ("source-changed", show with { ShotLayouts = [With("final-scores", new() { DataPin = new("headlines", 1) }, "leaderboard")] }, state, "final-scores", Entry("leaderboard"),
                new JsonObject { ["layoutRowSettings"] = new JsonObject { ["leaderboard"] = new JsonObject { ["dataPin"] = new JsonObject { ["sourceId"] = "headlines", ["revision"] = 1 } } } }),
            ("invalid-override", show, state, "bad-override", Entry("title"), null),
            // Pin, then overrides: a row with a stale pin AND an unusable override reports the pin.
            ("pin-checked-before-overrides", show with { ShotLayouts = [With("final-scores", new() { DataPin = new("headlines", 1),
                    Overrides = new Dictionary<string, JsonElement> { ["nope"] = JsonSerializer.SerializeToElement("x") } }, "leaderboard")] }, state, "final-scores", Entry("leaderboard"),
                new JsonObject { ["layoutRowSettings"] = new JsonObject { ["leaderboard"] = new JsonObject { ["dataPin"] = new JsonObject { ["sourceId"] = "headlines", ["revision"] = 1 }, ["overrides"] = new JsonObject { ["nope"] = "x" } } } }),
            ("template-missing", show, state with { Templates = state.Templates.Where(t => t.TemplateId != "centered-title").ToArray() }, "final-scores", Entry("title"),
                new JsonObject { ["dropTemplates"] = new JsonArray("centered-title") }),
            ("pin-hash-mismatch", show with { Templates = [pinHash] }, state, "final-scores", Entry("title"),
                new JsonObject { ["pins"] = JsonSerializer.SerializeToNode(new[] { pinHash }, Web) }),
            ("guest-override-on-a-compatibility-template", show with { ShotLayouts = [With("two-up", new() { Overrides = new Dictionary<string, JsonElement> { ["title"] = JsonSerializer.SerializeToElement("Guest") } }, "guest")] },
                state, "two-up", guest, new JsonObject { ["layoutRowSettings"] = new JsonObject { ["guest"] = new JsonObject { ["overrides"] = new JsonObject { ["title"] = "Guest" } } } }),
            ("guest-source-unavailable", show with { ShotLayouts = [With("two-up", new() { Overrides = new Dictionary<string, JsonElement> { ["title"] = JsonSerializer.SerializeToElement("Guest") } }, "guest")] },
                state, "two-up", guest with { SourceId = "zoom:999" }, new JsonObject { ["layoutRowSettings"] = new JsonObject { ["guest"] = new JsonObject { ["overrides"] = new JsonObject { ["title"] = "Guest" } } } }),
        };
        // The template catalog is written once (catalog-layout-state); a resolve row's node carries only the
        // ids it leaves out of it (dropTemplates) and a transcript step's node omits it (the replay keeps the
        // initial node's catalog), which keeps the file small.
        rows.Add(Row("catalog-layout-state", "catalog", new JsonObject(), new JsonObject { ["templates"] = JsonSerializer.SerializeToNode(state.Templates, Web) }));
        foreach (var (id, document, st, layout, entry, patch) in cases)
        {
            var node = JsonSerializer.SerializeToNode(st with { Templates = [] }, Web)!.AsObject();
            node.Remove("templates");
            rows.Add(Row("resolve-" + id, "resolve", new JsonObject
            {
                ["document"] = "documents/windows-layouts.json", ["patch"] = patch, ["layoutId"] = layout,
                ["entry"] = new JsonObject { ["rowId"] = entry.RowId, ["sourceId"] = entry.SourceId, ["name"] = entry.Label, ["title"] = entry.Title },
                ["node"] = node, ["templates"] = "catalog-layout-state",
                ["dropTemplates"] = new JsonArray(state.Templates.Select(t => t.TemplateId).Distinct().Where(t => st.Templates.All(x => x.TemplateId != t)).Select(t => (JsonNode)t).ToArray()),
            }, Resolve(document, st, layout, entry)));
        }

        // Cue transcripts.
        var other = new NativeGraphicsInstance { InstanceId = "playlist-sponsor", Revision = 2, CueRevision = 2, Cued = true, CuePreparation = "ready", TemplateId = "compact-solid", TemplateVersion = 1, Scope = "global" };
        var onAir = OnProgram("logo", "corner-logo");
        void Cue(string id, string layoutId, NativeGraphicsInstance[] instances, string script = "accept-all", bool cancelAfter = false)
        {
            var directory = Path.Combine(Path.GetTempPath(), "cvp-layout-transcript-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(directory);
            try
            {
                var initial = LayoutState(instances);
                var core = new TranscriptCore(initial) { Script = script };
                var path = Path.Combine(directory, "show.json");
                File.Copy(DocumentPath("windows-layouts.json"), path);
                var session = new GraphicsWorkspaceSession(core.Invoke, () => core.State); session.Observe(initial);
                var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(path), session);
                library.LoadAsync().GetAwaiter().GetResult();
                var service = new GraphicsPlayoutService(library, core.Invoke, () => core.State, core.Delay, () => core.Clock);
                Task<bool> Select(string sceneId, CancellationToken _) { core.Pseudo("scene-select", new JsonObject { ["sceneId"] = sceneId }); return Task.FromResult(script != "scene-missing"); }
                var result = service.CueLayoutAsync(layoutId, Select, guest, null, CancellationToken.None, () => "stamp").GetAwaiter().GetResult();
                var cancelled = cancelAfter ? service.CancelLayoutAsync().GetAwaiter().GetResult() : null;
                var steps = Regex.Replace(core.Steps.ToJsonString(), "layout-[0-9a-f]{32}", "ATTEMPT");
                var stepNodes = JsonNode.Parse(steps)!.AsArray();
                foreach (var step in stepNodes)
                {
                    (step!["nodeAfter"] as JsonObject)?.Remove("templates");
                    foreach (var poll in step["pollsAfter"] as JsonArray ?? []) (poll as JsonObject)?.Remove("templates");
                }
                steps = stepNodes.ToJsonString();
                var expected = new JsonObject
                {
                    ["steps"] = JsonNode.Parse(steps), ["accepted"] = result.Accepted, ["reason"] = result.Reason,
                    ["prepared"] = service.PreparedLayout is { } p ? new JsonObject { ["layoutId"] = p.Layout.Id,
                        ["cues"] = new JsonArray(p.Cues.Select(c => (JsonNode)new JsonObject { ["instanceId"] = c.InstanceId, ["cueRevision"] = c.CueRevision }).ToArray()) } : null,
                };
                if (cancelled is not null) expected["cancelReason"] = cancelled.Reason;
                rows.Add(Row("cue-" + id, "cueLayout", new JsonObject
                {
                    ["document"] = "documents/windows-layouts.json", ["layoutId"] = layoutId, ["script"] = script, ["cancelAfter"] = cancelAfter,
                    ["programSubject"] = new JsonObject { ["rowId"] = guest.RowId, ["sourceId"] = guest.SourceId, ["name"] = guest.Label, ["title"] = guest.Title, ["slotNumber"] = 1 },
                    ["initialNode"] = WithoutTemplates(initial), ["templates"] = "catalog-layout-state",
                }, expected));
            }
            finally { Directory.Delete(directory, true); }
        }
        Cue("legacy-open", "open", []);
        Cue("legacy-clean-plays-out-others", "clean", [onAir]);
        Cue("guarded-final-scores", "final-scores", []);
        Cue("guarded-second-row-fails", "final-scores", [], "fail-second-cue");
        Cue("guarded-required-instance-already-cued", "final-scores", [new() { InstanceId = "playlist-title", Revision = 2, CueRevision = 2, Cued = true, TemplateId = "centered-title", TemplateVersion = 1, Scope = "global" }]);
        Cue("guarded-over-capacity", "final-scores", Enumerable.Range(1, 7).Select(i => new NativeGraphicsInstance { InstanceId = $"other-{i}", Revision = 1, TemplateId = "compact-solid", TemplateVersion = 1, Scope = "global" }).ToArray());
        Cue("guarded-cancel-after-ready", "final-scores", [other], cancelAfter: true);
        Cue("missing-scene-refused", "open", [], "scene-missing");
        Check("layout-transcripts", "Shot Layouts: GraphicsLayoutResolver.Resolve (data pin, then overrides) and CueLayoutAsync / CancelLayoutAsync transcripts from the real GraphicsPlayoutService over documents/windows-layouts.json (ATTEMPT = the attempt id).", rows);
    }
}
