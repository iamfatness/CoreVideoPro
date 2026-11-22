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
public sealed class GraphicsShellVectorTests
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
}
