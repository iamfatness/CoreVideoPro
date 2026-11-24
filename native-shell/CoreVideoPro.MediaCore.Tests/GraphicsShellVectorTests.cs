using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Exporter for the Mac shell's graphics conformance vectors
/// (docs/superpowers/specs/2026-11-15-mac-graphics-shell-design.md section 5.2).
/// Each area file under contracts/graphics-shell/ holds rows {id, operation, input,
/// expected}; <c>expected</c> is computed HERE by the real MediaCore types, and this
/// test fails when the committed file differs, so a Windows rule cannot change
/// without the vectors changing in the same commit. The Mac shell replays every row
/// (COREVIDEO_SHELL_TESTS, ShellTestsGraphics.swift). Regenerate with
/// COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1.</summary>
public sealed partial class GraphicsShellVectorTests
{
    private static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);
    private static readonly JsonSerializerOptions Pretty = new()
    {
        WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    };

    internal static string AreaPath(string area) => ControlRepoPaths.Contract(Path.Combine("graphics-shell", area + ".vectors.json"));
    internal static string NodePath(string name) => ControlRepoPaths.Contract(Path.Combine("graphics-shell", "nodes", name));

    /// <summary>Writes (when asked) or verifies one area file.</summary>
    internal static void Check(string area, string description, JsonArray rows)
    {
        var document = new JsonObject
        {
            ["area"] = area,
            ["description"] = description,
            ["generatedBy"] = "native-shell/CoreVideoPro.MediaCore.Tests/GraphicsShellVectorTests.cs",
            ["rows"] = rows,
        };
        var text = document.ToJsonString(Pretty).Replace("\r\n", "\n") + "\n";
        var path = AreaPath(area);
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS") == "1")
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, text, new UTF8Encoding(false));
            return;
        }
        Assert.True(File.Exists(path), $"{path} is missing; run with COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1.");
        var committed = File.ReadAllText(path).Replace("\r\n", "\n");
        Assert.True(committed == text, $"{path} differs from what MediaCore computes. A Windows graphics rule changed: " +
            "regenerate with COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1 and update the Mac shell in the same commit.");
    }

    internal static JsonNode Row(string id, string operation, JsonNode input, JsonNode expected) =>
        new JsonObject { ["id"] = id, ["operation"] = operation, ["input"] = input, ["expected"] = expected };

    // ── gateway: GraphicsCommandRequest.TryBuild inside the Mac's thin pre-check scope ──

    private const string Epoch = "epoch-1";
    private static readonly string ObservedJson = """
        {"schemaVersion":1,"coreEpoch":"epoch-1",
         "instances":[{"instanceId":"featured","revision":3,"templateId":"compact-solid","templateVersion":1}],
         "templates":[{"templateId":"corporate-lower-third","templateVersion":1,"runtime":"declarative",
                       "fields":[{"id":"name","type":"string","maxLength":256},{"id":"title","type":"string"}]}]}
        """;

    private static string Request(string extra, string id = "req-1", string client = "graphics-playout", string epoch = Epoch,
        bool instance = true, bool revision = true)
    {
        var parts = new List<string> { "\"schemaVersion\":1", $"\"requestId\":\"{id}\"", $"\"clientId\":\"{client}\"", $"\"coreEpoch\":\"{epoch}\"" };
        if (instance) parts.Add("\"instanceId\":\"featured\"");
        if (revision) parts.Add("\"expectedRevision\":3");
        if (extra.Length > 0) parts.Add(extra);
        return "{" + string.Join(",", parts) + "}";
    }

    [Fact]
    public void GatewayVectors()
    {
        var observed = JsonSerializer.Deserialize<NativeGraphicsState>(ObservedJson, Web)!;
        var cases = new (string Id, string Action, string Request)[]
        {
            ("create-curated-global", "graphics.instance.create", Request("\"templateId\":\"compact-solid\",\"templateVersion\":1,\"scope\":\"global\",\"sceneId\":\"\",\"fields\":{\"name\":\"Ada\",\"title\":\"Host\"}", revision: false)),
            ("create-advertised-declarative", "graphics.instance.create", Request("\"templateId\":\"corporate-lower-third\",\"templateVersion\":1,\"scope\":\"global\",\"fields\":{\"name\":\"Ada\",\"title\":\"\"}", revision: false)),
            ("create-unknown-template", "graphics.instance.create", Request("\"templateId\":\"not-installed\",\"templateVersion\":1,\"scope\":\"global\"", revision: false)),
            ("create-scene-scope-without-scene", "graphics.instance.create", Request("\"templateId\":\"compact-solid\",\"templateVersion\":1,\"scope\":\"scene\"", revision: false)),
            ("create-global-with-scene", "graphics.instance.create", Request("\"templateId\":\"compact-solid\",\"templateVersion\":1,\"scope\":\"global\",\"sceneId\":\"s1\"", revision: false)),
            ("create-scene-scope", "graphics.instance.create", Request("\"templateId\":\"compact-solid\",\"templateVersion\":1,\"scope\":\"scene\",\"sceneId\":\"s1\"", revision: false)),
            ("unsupported-action", "graphics.instance.explode", Request("")),
            ("core-epoch-conflict", "graphics.instance.trigger", Request("\"action\":\"hide\"", epoch: "epoch-0")),
            ("unknown-field", "graphics.instance.trigger", Request("\"action\":\"hide\",\"colour\":\"red\"")),
            ("trigger-hide", "graphics.instance.trigger", Request("\"action\":\"hide\"")),
            ("trigger-show-from-cue", "graphics.instance.trigger", Request("\"target\":\"cue\",\"action\":\"show\"")),
            ("trigger-hide-with-target", "graphics.instance.trigger", Request("\"target\":\"cue\",\"action\":\"hide\"")),
            ("trigger-unknown-action", "graphics.instance.trigger", Request("\"action\":\"explode\"")),
            ("trigger-without-revision", "graphics.instance.trigger", Request("\"action\":\"hide\"", revision: false)),
            ("trigger-without-instance", "graphics.instance.trigger", Request("\"action\":\"hide\"", instance: false)),
            ("trigger-uncue-with-owner", "graphics.instance.trigger", Request("\"action\":\"uncue\",\"cueOwner\":\"layout-1\"")),
            ("trigger-hide-with-owner", "graphics.instance.trigger", Request("\"action\":\"hide\",\"cueOwner\":\"layout-1\"")),
            ("cue-ride-take", "graphics.instance.cue", Request("\"rideTake\":true")),
            ("cue-ride-take-not-bool", "graphics.instance.cue", Request("\"rideTake\":\"yes\"")),
            ("cue-with-owner", "graphics.instance.cue", Request("\"rideTake\":false,\"cueOwner\":\"layout-1\"")),
            ("update-cue", "graphics.instance.update", Request("\"target\":\"cue\",\"fields\":{\"name\":\"Bo\"}")),
            ("update-bad-target", "graphics.instance.update", Request("\"target\":\"wherever\",\"fields\":{\"name\":\"Bo\"}")),
            ("remove", "graphics.instance.remove", Request("")),
            ("program-clear-all", "graphics.program.clear", Request("\"channel\":\"all\"", instance: false, revision: false)),
            ("program-clear-some", "graphics.program.clear", Request("\"channel\":\"some\"", instance: false, revision: false)),
            ("authority-instances", "graphics.authority.set", Request("\"family\":\"lower-third\",\"authority\":\"instances\"", instance: false, revision: false)),
            ("authority-bad-family", "graphics.authority.set", Request("\"family\":\"titles\",\"authority\":\"instances\"", instance: false, revision: false)),
            ("template-list-thumbnails", "graphics.template.list", Request("\"includeThumbnails\":true", instance: false, revision: false)),
            ("bad-request-id", "graphics.instance.trigger", Request("\"action\":\"hide\"", id: "Bad_ID")),
            ("bad-client-id", "graphics.instance.trigger", Request("\"action\":\"hide\"", client: "Graphics Playout")),
            ("schema-version-2", "graphics.instance.trigger", Request("\"action\":\"hide\"").Replace("\"schemaVersion\":1", "\"schemaVersion\":2")),
            ("revision-zero", "graphics.instance.trigger", Request("\"action\":\"hide\"").Replace("\"expectedRevision\":3", "\"expectedRevision\":0")),
            ("revision-fraction", "graphics.instance.trigger", Request("\"action\":\"hide\"").Replace("\"expectedRevision\":3", "\"expectedRevision\":1.5")),
            ("oversize", "graphics.instance.update", Request("\"target\":\"cue\",\"fields\":{\"name\":\"" + new string('x', 40000) + "\"}")),
            ("instance-id-too-long", "graphics.instance.trigger", Request("\"action\":\"hide\"").Replace("\"featured\"", "\"" + new string('a', 65) + "\"")),
            ("duplicate-field", "graphics.instance.trigger", Request("\"action\":\"hide\",\"action\":\"show\"")),
            ("first-fault-wins-unknown-then-bad-type", "graphics.instance.trigger", Request("\"bogus\":true,\"action\":5")),
            ("first-fault-wins-bad-type-then-unknown", "graphics.instance.trigger", Request("\"action\":5,\"bogus\":true")),
            ("rideTake-on-trigger", "graphics.instance.trigger", Request("\"action\":\"hide\",\"rideTake\":true")),
            ("string-with-nul", "graphics.instance.update", Request("\"target\":\"cue\",\"label\":\"a\\u0000b\"")),
        };
        var rows = new JsonArray();
        foreach (var (id, action, request) in cases)
        {
            var ok = GraphicsCommandRequest.TryBuild(action, request, observed, out var command, out var requestId, out var error);
            var expected = new JsonObject { ["ok"] = ok };
            if (ok)
            {
                expected["type"] = command!.Type;
                expected["graphics"] = JsonNode.Parse(command.ExtensionData!["graphics"].GetRawText());
            }
            else
            {
                expected["error"] = error;
                expected["requestId"] = requestId;
            }
            rows.Add(Row(id, "tryBuild", new JsonObject { ["action"] = action, ["request"] = request, ["observed"] = JsonNode.Parse(ObservedJson) }, expected));
        }
        Check("gateway", "GraphicsCommandRequest.TryBuild results for requests inside the Mac shell's thin pre-check (plan D1).", rows);
    }

    // ── state: decode a graphics node and project the On Air strip ──────────────────

    internal static JsonObject StripJson(GraphicsStripState strip)
    {
        JsonArray Items(IEnumerable<GraphicsStripItem> items) => new(items.Select(i => (JsonNode)new JsonObject
        {
            ["instanceId"] = i.InstanceId, ["label"] = i.Label, ["owner"] = i.Owner, ["rowId"] = i.RowId, ["sourceId"] = i.SourceId,
            ["revision"] = i.Revision, ["visibility"] = i.Visibility, ["cuePending"] = i.CuePending,
        }).ToArray());
        return new JsonObject { ["coreEpoch"] = strip.CoreEpoch, ["onProgram"] = Items(strip.OnProgram), ["onPreview"] = Items(strip.OnPreview) };
    }

    internal static GraphicsPlayoutService Playout(IReadOnlyList<GraphicsPlaylistRow> rows, NativeGraphicsState state, string directory)
    {
        var session = new GraphicsWorkspaceSession((_, _, _) => Task.FromResult(new NativeGraphicsOperationResult()), () => state);
        session.Observe(state);
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(Path.Combine(directory, "show.json")), session);
        if (rows.Count > 0) Assert.True(library.SavePlaylistAsync(rows).GetAwaiter().GetResult(), library.Status);
        return new GraphicsPlayoutService(library, (_, _, _) => Task.FromResult(new NativeGraphicsOperationResult()), () => state);
    }

    internal static readonly (string Id, string Node)[] SyntheticNodes =
    {
        ("building-out-and-show-pending", """
            {"schemaVersion":1,"coreEpoch":"e1","instances":[
              {"instanceId":"a","revision":2,"programRevision":2,"visibility":"building-out","templateId":"compact-solid","fields":{"name":"Out"}},
              {"instanceId":"b","revision":4,"programRevision":3,"visibility":"on-air","templateId":"compact-solid","label":"Rack label","fields":{"name":"On"}},
              {"instanceId":"c","revision":1,"programRevision":0,"visibility":"hidden","showPending":true,"templateId":"centered-title","fields":{}},
              {"instanceId":"d","revision":5,"programRevision":5,"visibility":"hidden","templateId":"compact-solid","fields":{"name":"Gone"}},
              {"instanceId":"e","revision":6,"programRevision":0,"cueRevision":6,"cuePending":true,"visibility":"hidden","templateId":"corner-logo","fields":{}}
            ]}
            """),
        ("social-cue-hidden-from-preview", """
            {"schemaVersion":1,"coreEpoch":"e2","instances":[
              {"instanceId":"social-review-card","revision":1,"cueRevision":1,"visibility":"hidden","templateId":"youtube-chat-card","fields":{}},
              {"instanceId":"playlist-guest-3f1c","revision":2,"cueRevision":2,"visibility":"hidden","templateId":"compact-solid","fields":{"name":"Ada"}}
            ]}
            """),
        ("unavailable-schema-0", """{"schemaVersion":0,"coreEpoch":"","instances":[]}"""),
    };

    [Fact]
    public void StateVectors()
    {
        var directory = Path.Combine(Path.GetTempPath(), "cvp-graphics-vectors-" + Guid.NewGuid().ToString("N"));
        try
        {
            var rows = new JsonArray();
            var inputs = new List<(string Id, JsonNode Input, string Text)>();
            foreach (var file in new[] { "empty.json", "program-and-cue.json" })
                inputs.Add(("node-" + Path.GetFileNameWithoutExtension(file), new JsonObject { ["nodeFile"] = "nodes/" + file }, File.ReadAllText(NodePath(file))));
            foreach (var (id, node) in SyntheticNodes)
                inputs.Add(("synthetic-" + id, new JsonObject { ["node"] = JsonNode.Parse(node) }, node));
            foreach (var (id, input, text) in inputs)
            {
                var state = JsonSerializer.Deserialize<NativeGraphicsState>(text, Web)!;
                var available = state.SchemaVersion == 1 && state.CoreEpoch.Length > 0;
                var playout = Playout([], state, directory);
                playout.Observe(state);
                var expected = new JsonObject
                {
                    ["available"] = available,
                    ["instanceCount"] = state.Instances.Count,
                    ["templateCount"] = state.Templates.Count,
                    ["strip"] = StripJson(playout.Strip),
                };
                rows.Add(Row(id, "decodeAndProjectStrip", input, expected));
            }
            Check("state", "NativeGraphicsState decode + GraphicsPlayoutService strip projection (no playlist rows).", rows);
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, true); }
    }

    // ── document: playlist row validity + GraphicsDocumentStore load outcomes ──────

    internal static NativeGraphicsState RealCatalog() =>
        JsonSerializer.Deserialize<NativeGraphicsState>(File.ReadAllText(NodePath("program-and-cue.json")), Web)!;

    internal static IReadOnlyList<GraphicsPlaylistRow> InterviewRows() =>
        GraphicsWorkspaceLibrary.DefaultPlaylistFor(GraphicsShowStarters.All.First(s => s.Id == "interview"), RealCatalog());

    private static readonly JsonSerializerOptions Store = new(JsonSerializerDefaults.Web) { WriteIndented = true, MaxDepth = 32 };

    internal static string DocumentPath(string name) => ControlRepoPaths.Contract(Path.Combine("graphics-shell", "documents", name));

    /// <summary>A show document written by the real Windows store: default interview rows,
    /// a saved cue, a template pin, a sources column, a shot layout and an unknown future
    /// section. The Mac must load it and re-save it without losing anything.</summary>
    internal static string WindowsShowDocument()
    {
        var catalog = RealCatalog();
        var graphic = GraphicsShowStarters.Prepare(GraphicsShowStarters.All.First(s => s.Id == "interview"), catalog)[1];
        var definition = JsonSerializer.SerializeToElement(new { templateId = "centered-title", templateVersion = 1, runtime = "declarative" });
        var document = new GraphicsShowDocument
        {
            ShowId = "default", Authority = "legacy", Playlist = InterviewRows(),
            Cues = [new GraphicsSavedCue { Id = "welcome", Name = "Welcome", Draft = graphic.Draft }],
            Templates = [new GraphicsSavedTemplate("centered-title", 1, new string('a', 64), definition)],
            SourceColumns = [new GraphicsSourceColumn("pronouns", "Pronouns", new Dictionary<int, string> { [1] = "they/them" })],
            ShotLayouts = GraphicsWorkspaceLibrary.DefaultShotLayouts(),
            Metadata = new() { ["futureSection"] = JsonSerializer.SerializeToElement(new { keep = true, nested = new[] { 1, 2 } }) },
        };
        return Encoding.UTF8.GetString(JsonSerializer.SerializeToUtf8Bytes(document, Store)).Replace("\r\n", "\n") + "\n";
    }

    [Fact]
    public void DocumentVectors()
    {
        // The fixture document itself is regenerated / verified like an area file.
        var windows = WindowsShowDocument();
        var fixture = DocumentPath("windows-show.json");
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS") == "1")
        { Directory.CreateDirectory(Path.GetDirectoryName(fixture)!); File.WriteAllText(fixture, windows, new UTF8Encoding(false)); }
        else Assert.True(File.Exists(fixture) && File.ReadAllText(fixture).Replace("\r\n", "\n") == windows, $"{fixture} differs; regenerate.");

        var rows = new JsonArray();
        var defaults = InterviewRows();
        JsonNode Ser(object o) => JsonSerializer.SerializeToNode(o, Web)!;
        var guest = Ser(defaults[0]).AsObject();
        JsonObject With(Action<JsonObject> change) { var copy = guest.DeepClone().AsObject(); change(copy); return copy; }
        var rowCases = new List<(string Id, JsonNode Row)>();
        for (var i = 0; i < defaults.Count; i++) rowCases.Add(("default-" + defaults[i].Id, Ser(defaults[i])));
        rowCases.AddRange(new (string, JsonNode)[] {
            ("minimal", JsonNode.Parse("""{"id":"a","name":"A"}""")!),
            ("bad-id-uppercase", With(r => r["id"] = "Bad")),
            ("id-49-chars", With(r => r["id"] = new string('a', 49))),
            ("id-48-chars", With(r => r["id"] = new string('a', 48))),
            ("unknown-kind", With(r => r["kind"] = "crawl")),
            ("empty-name", With(r => r["name"] = "")),
            ("name-129", With(r => r["name"] = new string('n', 129))),
            ("name-wrong-type", With(r => r["name"] = 5)),
            ("hotkey-f13", With(r => r["hotkey"] = "F13")),
            ("hotkey-f0", With(r => r["hotkey"] = "F0")),
            ("hotkey-lower", With(r => r["hotkey"] = "f1")),
            ("hotkey-f12", With(r => r["hotkey"] = "F12")),
            ("play-out-5000", With(r => r["playOutAfterMs"] = 5000)),
            ("play-out-15000", With(r => r["playOutAfterMs"] = 15000)),
            ("template-without-version", With(r => r["templateVersion"] = 0)),
            ("version-without-template", With(r => { r["templateId"] = ""; r["templateVersion"] = 1; })),
            ("template-version-fraction", With(r => r["templateVersion"] = 1.5)),
            ("style-center-anchor", With(r => r["style"]!["anchor"] = "center")),
            ("style-name-size-200", With(r => r["style"]!["nameSize"] = 200)),
            ("style-bad-colour", With(r => r["style"]!["nameColor"] = "#12345")),
            ("style-missing-uses-default", With(r => r.Remove("style"))),
            ("motion-ok", With(r => r["motion"] = JsonNode.Parse("""{"layers":"whole","in":{"type":"slide","direction":"up","easing":"ease-out","durationMs":300},"out":{"type":"fade","durationMs":200}}"""))),
            ("motion-bad-type", With(r => r["motion"] = JsonNode.Parse("""{"in":{"type":"zoom"},"out":{}}"""))),
            ("motion-too-long", With(r => r["motion"] = JsonNode.Parse("""{"in":{"durationMs":3001},"out":{}}"""))),
            ("variants-ok", With(r => r["variants"] = JsonNode.Parse("""[{"id":"v1","name":"Ada","title":"Host"},{"id":"v2","name":"Bo"}]"""))),
            ("variants-duplicate-ids", With(r => r["variants"] = JsonNode.Parse("""[{"id":"v1","name":"Ada"},{"id":"v1","name":"Bo"}]"""))),
            ("variant-title-257", With(r => r["variants"] = JsonNode.Parse("[{\"id\":\"v1\",\"name\":\"Ada\",\"title\":\"" + new string('t', 257) + "\"}]"))),
            ("placement-ok", With(r => r["placement"] = JsonNode.Parse("""{"anchor":"upper-center","x":0,"y":10,"scale":1.5}"""))),
            ("placement-scale-5", With(r => r["placement"] = JsonNode.Parse("""{"anchor":"center","x":0,"y":0,"scale":5}"""))),
            ("clock-countdown", With(r => r["clock"] = JsonNode.Parse("""{"mode":"countdown","zone":"utc","format":"hm","finish":"hide","offsetMinutes":0,"durationSeconds":600}"""))),
            ("clock-bad-mode", With(r => r["clock"] = JsonNode.Parse("""{"mode":"stopwatch"}"""))),
            ("image-ok", With(r => r["image"] = JsonNode.Parse("""{"templateId":"logo-pack","templateVersion":1,"assetId":"logo"}"""))),
            ("image-missing-asset", With(r => r["image"] = JsonNode.Parse("""{"templateId":"logo-pack","templateVersion":1,"assetId":""}"""))),
            ("fields-33", With(r => { var f = new JsonObject(); for (var k = 0; k < 33; k++) f["f" + k] = "x"; r["fields"] = f; })),
            ("source-mapping-bad-key", With(r => r["sourceMappings"] = JsonNode.Parse("""{"Name":"name"}"""))),
            ("source-mapping-ok", With(r => r["sourceMappings"] = JsonNode.Parse("""{"name":"pronouns"}"""))),
            ("image-path-nul", With(r => r["imagePath"] = "a\u0000b")),
        });
        foreach (var (id, row) in rowCases)
        {
            bool valid;
            try { valid = JsonSerializer.Deserialize<GraphicsPlaylistRow>(row.ToJsonString(), Web)?.IsValid == true; }
            catch (JsonException) { valid = false; }
            rows.Add(Row("row-" + id, "validateRow", new JsonObject { ["row"] = row.DeepClone() }, new JsonObject { ["valid"] = valid }));
        }

        var directory = Path.Combine(Path.GetTempPath(), "cvp-graphics-doc-vectors-" + Guid.NewGuid().ToString("N"));
        try
        {
            var minimal = """{"schemaVersion":3}""";
            var loads = new (string Id, string? Primary, string? Backup)[] {
                ("missing", null, null),
                ("minimal-v3", minimal, null),
                ("v1-upgrades", """{"schemaVersion":1,"showId":"old"}""", null),
                ("v2-upgrades", """{"schemaVersion":2,"playlist":[{"id":"a","name":"A"}]}""", null),
                ("v4-read-only", """{"schemaVersion":4,"playlist":[]}""", null),
                ("schema-0", """{"schemaVersion":0}""", null),
                ("schema-missing", """{"showId":"x"}""", null),
                ("schema-string", """{"schemaVersion":"3"}""", null),
                ("duplicate-nested-key", """{"schemaVersion":3,"playlist":[{"id":"a","name":"A","name":"B"}]}""", null),
                ("duplicate-top-key", """{"schemaVersion":3,"showId":"a","showId":"b"}""", null),
                ("authority-volt", """{"schemaVersion":3,"authority":"volt"}""", null),
                ("authority-bad", """{"schemaVersion":3,"authority":"instances"}""", null),
                ("show-id-blank", """{"schemaVersion":3,"showId":"  "}""", null),
                ("show-id-65", "{\"schemaVersion\":3,\"showId\":\"" + new string('s', 65) + "\"}", null),
                ("playlist-duplicate-ids", """{"schemaVersion":3,"playlist":[{"id":"a","name":"A"},{"id":"a","name":"B"}]}""", null),
                ("playlist-invalid-row", """{"schemaVersion":3,"playlist":[{"id":"a","name":""}]}""", null),
                ("cues-65", "{\"schemaVersion\":3,\"cues\":[" + string.Join(",", Enumerable.Repeat("{}", 65)) + "]}", null),
                ("template-bad-hash", """{"schemaVersion":3,"templates":[{"templateId":"t","templateVersion":1,"manifestHash":"xyz","definition":{}}]}""", null),
                ("template-ok", "{\"schemaVersion\":3,\"templates\":[{\"templateId\":\"t\",\"templateVersion\":1,\"manifestHash\":\"" + new string('b', 64) + "\",\"definition\":{},\"packageRoot\":\"\"}]}", null),
                ("unknown-section-kept", """{"schemaVersion":3,"futureSection":{"a":1}}""", null),
                ("corrupt-primary-good-backup", """{"schemaVersion":3,"playlist":[{"id":"a","name":""}]}""", """{"schemaVersion":3,"playlist":[{"id":"b","name":"B"}]}"""),
                ("no-primary-good-backup", null, """{"schemaVersion":3,"playlist":[{"id":"b","name":"B"}]}"""),
                ("both-invalid", """{"schemaVersion":0}""", """{"showId":"x"}"""),
                ("backup-duplicate-key", """{"schemaVersion":0}""", """{"schemaVersion":3,"a":1,"a":2}"""),
                ("backup-invalid-playlist", """{"schemaVersion":0}""", """{"schemaVersion":3,"playlist":[{"id":"a","name":""}]}"""),
                ("windows-show", windows, null),
            };
            foreach (var (id, primary, backup) in loads)
            {
                var path = Path.Combine(directory, id, "default.json");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                if (primary is not null) File.WriteAllText(path, primary, new UTF8Encoding(false));
                if (backup is not null) File.WriteAllText(path + ".bak", backup, new UTF8Encoding(false));
                var load = new GraphicsDocumentStore(path).LoadAsync().GetAwaiter().GetResult();
                var expected = new JsonObject
                {
                    ["readOnly"] = load.ReadOnly, ["recoveredBackup"] = load.RecoveredBackup, ["status"] = load.Status,
                    ["showId"] = load.Document.ShowId, ["authority"] = load.Document.Authority,
                    ["playlistIds"] = new JsonArray(load.Document.Playlist.Select(r => (JsonNode)r.Id).ToArray()),
                };
                var input = new JsonObject();
                if (id == "windows-show") input["primaryFile"] = "documents/windows-show.json"; else if (primary is not null) input["primary"] = primary;
                if (backup is not null) input["backup"] = backup;
                rows.Add(Row("load-" + id, "load", input, expected));
            }
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, true); }
        Check("document", "GraphicsPlaylistRow.IsValid and GraphicsDocumentStore.LoadAsync outcomes (plan D3: unported sections are counted, not re-validated).", rows);
    }

    // ── playout: identity, entries, cue fields, style wire, default rows ───────────

    [Fact]
    public void PlayoutVectors()
    {
        var rows = new JsonArray();
        foreach (var (row, source) in new[] { ("guest", ""), ("guest", "zoom:16778240"), ("title", "capture:Cámara 1"),
                     ("guest", "media:clip-1"), ("logo", "zoom:🎙️ host"), ("live-bug", "variant:v1") })
            rows.Add(Row($"instance-id-{row}-{rows.Count}", "instanceIdFor", new JsonObject { ["rowId"] = row, ["sourceId"] = source },
                new JsonObject { ["instanceId"] = GraphicsPlayoutService.InstanceIdFor(row, source) }));

        var guestRow = new GraphicsPlaylistRow { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest name", FollowSources = true };
        var slots = new[] {
            new GraphicsRosterSlot(3, "zoom:300", "Cy"), new GraphicsRosterSlot(1, "zoom:100", "Ada"), new GraphicsRosterSlot(2, null, "Empty"),
            new GraphicsRosterSlot(4, "zoom:100", "Ada again"), new GraphicsRosterSlot(5, "capture:cam", "Desk cam"), new GraphicsRosterSlot(6, "", "Blank"),
        };
        var entries = GraphicsPlayoutService.GuestEntries(guestRow, slots, (_, name) => (name, ""));
        rows.Add(Row("guest-entries", "guestEntries",
            new JsonObject { ["rowId"] = "guest", ["slots"] = new JsonArray(slots.Select(s => (JsonNode)new JsonObject {
                ["slotNumber"] = s.SlotNumber, ["sourceId"] = s.SourceId, ["derivedName"] = s.DerivedName }).ToArray()) },
            new JsonObject { ["entries"] = new JsonArray(entries.Select(e => (JsonNode)new JsonObject {
                ["sourceId"] = e.SourceId, ["name"] = e.Label, ["title"] = e.Title, ["slotNumber"] = e.SlotNumber, ["instanceId"] = GraphicsPlayoutService.InstanceIdFor(e) }).ToArray()) }));

        var catalog = RealCatalog();
        var defaults = InterviewRows();
        foreach (var (rowIndex, sourceId, name, title) in new[] { (0, "zoom:100", "Ada Lovelace", ""), (0, "", "", ""), (1, "", "", ""),
                     (1, "zoom:100", "Ada", "Host"), (3, "", "", "") })
        {
            var row = defaults[rowIndex];
            var template = catalog.Templates.First(t => t.TemplateId == row.TemplateId && t.TemplateVersion == row.TemplateVersion);
            var entry = sourceId.Length == 0 ? GraphicsPlayoutService.EntryFor(row) : new GraphicsPlaylistEntry(row.Id, sourceId, name, title) { SlotNumber = 1 };
            var fields = GraphicsLayoutResolver.CueFields(row, entry, template.Runtime == "declarative", template.Fields, []);
            var draft = new NativeGraphicsDraft(row.TemplateId, row.TemplateVersion, JsonSerializer.SerializeToElement(fields), row.Style, new());
            rows.Add(Row($"cue-fields-{row.Id}-{(sourceId.Length == 0 ? "row" : "guest")}-{rows.Count}", "cueFieldsAndStyle",
                new JsonObject { ["row"] = JsonSerializer.SerializeToNode(row, Web), ["entry"] = new JsonObject {
                    ["sourceId"] = entry.SourceId, ["name"] = entry.Label, ["title"] = entry.Title },
                    ["templateId"] = template.TemplateId, ["templateVersion"] = template.TemplateVersion },
                new JsonObject { ["fields"] = JsonSerializer.SerializeToNode(fields, Web), ["style"] = JsonNode.Parse(draft.StyleWire().GetRawText()) }));
        }
        rows.Add(Row("default-rows-interview", "defaultRows", new JsonObject { ["starter"] = "interview", ["nodeFile"] = "nodes/program-and-cue.json" },
            new JsonObject { ["rows"] = JsonSerializer.SerializeToNode(defaults, Web) }));
        Check("playout", "GraphicsPlayoutService identity and entries, GraphicsLayoutResolver.CueFields (no Sources columns), StyleWire, and the interview default rows.", rows);
    }

    // ── review round: names lists, clock rows, data rows ────────────────────────────

    /// A names-list row (logo-accent, three lines, cells for text-size incl. an out-of-range
    /// and a non-number cell) and a countdown clock row, built from the real catalog.
    internal static (GraphicsPlaylistRow Names, GraphicsPlaylistRow Clock) NamesAndClockRows()
    {
        var catalog = RealCatalog();
        var accent = catalog.Templates.First(t => t.TemplateId == "logo-accent");
        var clockTemplate = catalog.Templates.First(t => t.TemplateId == "clock-panel");
        var names = new GraphicsPlaylistRow {
            Id = "panel-names", Kind = GraphicsPlaylistRow.Title, Name = "Panel names", Hotkey = "F6",
            TemplateId = accent.TemplateId, TemplateVersion = accent.TemplateVersion,
            Fields = new Dictionary<string, JsonElement>(StringComparer.Ordinal) {
                ["name"] = JsonSerializer.SerializeToElement("Row default"), ["title"] = JsonSerializer.SerializeToElement(""),
                ["text-size"] = JsonSerializer.SerializeToElement(42) },
            Variants = [
                new GraphicsPlaylistVariant { Id = "v-ada", Name = "Ada Lovelace", Title = "Mathematician",
                    Cells = new Dictionary<string, string>(StringComparer.Ordinal) { ["text-size"] = "72" } },
                new GraphicsPlaylistVariant { Id = "v-bo", Name = "Bo Diddley", Title = "",
                    Cells = new Dictionary<string, string>(StringComparer.Ordinal) { ["text-size"] = "500" } },
                new GraphicsPlaylistVariant { Id = "v-cy", Name = "Cy Twombly", Title = "Painter",
                    Cells = new Dictionary<string, string>(StringComparer.Ordinal) { ["text-size"] = "big", ["show-logo"] = "false" } },
            ],
        };
        var clock = new GraphicsPlaylistRow {
            Id = "countdown", Kind = GraphicsPlaylistRow.LiveBug, Name = "Countdown", Hotkey = "F7",
            TemplateId = clockTemplate.TemplateId, TemplateVersion = clockTemplate.TemplateVersion, ShowClock = true,
            Clock = new NativeGraphicsClockConfiguration { Mode = "countdown", Zone = "utc", Format = "hm", Finish = "hide", DurationSeconds = 600 },
        };
        return (names, clock);
    }

    internal static string WindowsNamesClockDocument()
    {
        var (names, clock) = NamesAndClockRows();
        var document = new GraphicsShowDocument { Playlist = [names, clock] };
        return Encoding.UTF8.GetString(JsonSerializer.SerializeToUtf8Bytes(document, Store)).Replace("\r\n", "\n") + "\n";
    }

    [Fact]
    public void NamesListAndClockVectors()
    {
        var text = WindowsNamesClockDocument();
        var fixture = DocumentPath("windows-names-clock.json");
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS") == "1")
        { Directory.CreateDirectory(Path.GetDirectoryName(fixture)!); File.WriteAllText(fixture, text, new UTF8Encoding(false)); }
        else Assert.True(File.Exists(fixture) && File.ReadAllText(fixture).Replace("\r\n", "\n") == text, $"{fixture} differs; regenerate.");

        var (names, clock) = NamesAndClockRows();
        var catalog = RealCatalog();
        var accent = catalog.Templates.First(t => t.TemplateId == names.TemplateId);
        var rows = new JsonArray();
        var entries = GraphicsPlayoutService.VariantEntries(names);
        rows.Add(Row("variant-entries", "variantEntries", new JsonObject { ["rowId"] = names.Id },
            new JsonObject { ["entries"] = new JsonArray(entries.Select(e => (JsonNode)new JsonObject {
                ["sourceId"] = e.SourceId, ["name"] = e.Label, ["title"] = e.Title, ["instanceId"] = GraphicsPlayoutService.InstanceIdFor(e) }).ToArray()) }));
        foreach (var (id, onAir) in new[] { ("none", Array.Empty<string>()), ("first", new[] { "variant:v-ada" }),
                     ("last-wraps", new[] { "variant:v-cy" }), ("two-on-air-takes-the-later", new[] { "variant:v-ada", "variant:v-bo" }) })
            rows.Add(Row("next-variant-" + id, "nextVariant", new JsonObject { ["rowId"] = names.Id, ["onProgram"] = new JsonArray(onAir.Select(s => (JsonNode)s).ToArray()) },
                new JsonObject { ["sourceId"] = GraphicsPlayoutService.NextVariant(names, onAir)?.SourceId }));
        foreach (var entry in entries.Append(GraphicsPlayoutService.EntryFor(names)))
        {
            var fields = GraphicsLayoutResolver.CueFields(names, entry, true, accent.Fields, []);
            rows.Add(Row("variant-cue-fields-" + (entry.SourceId.Length == 0 ? "row" : entry.SourceId[8..]), "cueFields",
                new JsonObject { ["rowId"] = names.Id, ["sourceId"] = entry.SourceId, ["name"] = entry.Label, ["title"] = entry.Title },
                new JsonObject { ["fields"] = JsonSerializer.SerializeToNode(fields, Web) }));
        }
        rows.Add(Row("clock-configuration", "clockConfiguration", new JsonObject { ["rowId"] = clock.Id },
            new JsonObject { ["configuration"] = JsonSerializer.SerializeToNode(clock.Clock, Web), ["valid"] = clock.Clock!.IsValid }));
        Check("names-clock", "Names-list expansion (VariantEntries / NextVariant), variant cue fields with cells (GraphicsMappedValues), and the clock configuration a clock row sends; rows from documents/windows-names-clock.json.", rows);
    }
}
