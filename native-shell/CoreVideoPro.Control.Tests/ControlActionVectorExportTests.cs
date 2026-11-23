using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace CoreVideoPro.Control.Tests;

/// <summary>
/// Writes (or checks) this host's sections of <c>contracts/control.action-vectors.json</c>: per-action
/// inputs run through the REAL Windows helper, with the outcome the helper produced. The macOS shell
/// replays every case (mac-shell ControlApiTestsRC7.swift), so a refusal sentence is exported from the
/// Windows code, never retyped. Plan: docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md
/// Task 1. <c>COREVIDEO_REWRITE_ACTION_VECTORS=1</c> rewrites this writer's sections only; other writers'
/// sections (WT, MCT) are kept byte for byte.
/// </summary>
public sealed class ControlActionVectorExportTests
{
    internal const string RewriteSwitch = "COREVIDEO_REWRITE_ACTION_VECTORS";

    /// <summary>The <c>scene.cueWithRoutes</c> parse inputs (plan Task 1 step 1), as fixture constants.</summary>
    private static readonly (string Name, string Routes)[] ParseInputs =
    [
        ("parse-object-slot", """{"cam":2}"""),
        ("parse-object-null", """{"cam":null}"""),
        ("parse-array", "[1]"),
        ("parse-string", "\"x\""),
        ("parse-empty-key", """{"":1}"""),
        ("parse-string-slot", """{"cam":"2"}"""),
        ("parse-fractional-slot", """{"cam":1.5}"""),
        ("parse-not-json", "not json"),
        // Added beyond the plan's list: the remaining shapes the Mac parser must agree on.
        ("parse-blank", "   "),
        ("parse-empty-object", "{}"),
        ("parse-bool-slot", """{"cam":true}"""),
        ("parse-exponent-slot", """{"cam":1e2}"""),
        ("parse-int32-overflow", """{"cam":3000000000}"""),
        ("parse-two-routes", """{"a":1,"b":null}"""),
    ];

    /// <summary>
    /// <c>SceneCueWithRoutesArgs.Refuse</c> inputs. Fixture invariant: the known lists below are the
    /// <c>given</c> of every refuse case, slot 11 is NOT a known slot, and route <c>b</c> is not a known
    /// route of <c>two-up</c>.
    /// </summary>
    private static readonly string[] KnownScenes = ["two-up", "solo"];
    private static readonly string[] KnownRoutes = ["two-up-layer-0", "two-up-layer-1"];
    private static readonly int[] KnownSlots = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

    private static readonly (string Name, string SceneId, string Routes)[] RefuseInputs =
    [
        ("refuse-none", "two-up", """{"two-up-layer-0":1,"two-up-layer-1":null}"""),
        ("refuse-unknown-scene", "gone", """{"two-up-layer-0":1}"""),
        ("refuse-blank-scene", "", "{}"),
        ("refuse-unknown-route", "two-up", """{"two-up-layer-0":1,"b":2}"""),
        ("refuse-unknown-slot", "two-up", """{"two-up-layer-0":11}"""),
        // Route check runs before slot check (Windows order): both wrong -> the route sentence.
        ("refuse-route-before-slot", "two-up", """{"two-up-layer-0":11,"b":1}"""),
    ];

    internal static JsonObject SceneRoutesSection()
    {
        var cases = new JsonArray();
        foreach (var (name, raw) in ParseInputs)
        {
            var ok = SceneCueWithRoutesArgs.TryParseRoutes(raw, out var routes, out var error);
            var expect = new JsonObject { ["ok"] = ok };
            if (ok)
            {
                var map = new JsonObject();
                foreach (var (route, slot) in routes) map[route] = slot is { } value ? JsonValue.Create(value) : null;
                expect["effects"] = new JsonObject { ["routes"] = map };
            }
            else
            {
                expect["error"] = error;
                // System.Text.Json's exception text is runtime wording no other parser reproduces;
                // the replay matches everything up to and including the stable prefix.
                const string notJson = "routes is not JSON: ";
                if (error!.StartsWith(notJson, StringComparison.Ordinal)) expect["errorPrefix"] = notJson;
            }
            cases.Add(new JsonObject
            {
                ["name"] = name,
                ["action"] = "scene.cueWithRoutes",
                ["args"] = new JsonArray("two-up", raw),
                ["given"] = new JsonObject { ["step"] = "parse" },
                ["expect"] = expect,
            });
        }

        foreach (var (name, sceneId, raw) in RefuseInputs)
        {
            Assert.True(SceneCueWithRoutesArgs.TryParseRoutes(raw, out var routes, out _), $"{name} parses");
            var error = SceneCueWithRoutesArgs.Refuse(sceneId, KnownScenes, KnownRoutes, KnownSlots, routes);
            var expect = new JsonObject { ["ok"] = error is null };
            if (error is not null) expect["error"] = error;
            cases.Add(new JsonObject
            {
                ["name"] = name,
                ["action"] = "scene.cueWithRoutes",
                ["args"] = new JsonArray(sceneId, raw),
                ["given"] = new JsonObject
                {
                    ["step"] = "refuse",
                    ["scenes"] = new JsonArray(KnownScenes.Select(s => (JsonNode?)s).ToArray()),
                    ["routeIds"] = new JsonArray(KnownRoutes.Select(s => (JsonNode?)s).ToArray()),
                    ["slots"] = new JsonArray(KnownSlots.Select(s => (JsonNode?)s).ToArray()),
                },
                ["expect"] = expect,
            });
        }

        return new JsonObject
        {
            ["source"] = "native-shell/CoreVideoPro.Control/SceneCueWithRoutesArgs.cs:10,74",
            ["writer"] = "CT",
            ["cases"] = cases,
        };
    }

    [Fact]
    public void SceneRoutesSectionIsCurrent() =>
        ActionVectorFile.CheckOrRewrite(RepoPaths.Contract("control.action-vectors.json"), "scene-routes", SceneRoutesSection());

    [Fact]
    public void ParseVectorsCarryTheWindowsSentences()
    {
        // Fixture-derived operands (rule 2): two sentences checked by hand against the helper's source.
        var section = SceneRoutesSection();
        var cases = section["cases"]!.AsArray();
        string? Error(string name) => cases.Single(c => (string?)c!["name"] == name)!["expect"]!["error"]?.GetValue<string>();
        Assert.Equal("routes 'cam' must be an integer slot or null.", Error("parse-fractional-slot"));
        Assert.Equal("Unknown Show Input slot 11 for route 'two-up-layer-0'.", Error("refuse-unknown-slot"));
        Assert.StartsWith("routes is not JSON: ", Error("parse-not-json"));
    }
}

/// <summary>
/// Reads and writes one section of the shared action-vector file. Duplicated (deliberately small) in
/// CoreVideoPro.WinUI.Tests and CoreVideoPro.MediaCore.Tests: the three test assemblies share no code.
/// </summary>
internal static class ActionVectorFile
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static void CheckOrRewrite(string path, string sectionName, JsonObject expected)
    {
        var root = File.Exists(path)
            ? JsonNode.Parse(File.ReadAllText(path))!.AsObject()
            : new JsonObject { ["version"] = 1, ["sections"] = new JsonObject() };
        var sections = root["sections"]!.AsObject();
        if (Environment.GetEnvironmentVariable(ControlActionVectorExportTests.RewriteSwitch) == "1")
        {
            sections[sectionName] = expected.DeepClone();
            File.WriteAllText(path, root.ToJsonString(Options).Replace("\r\n", "\n") + "\n", new UTF8Encoding(false));
            return;
        }
        var actual = sections[sectionName];
        Assert.True(actual is not null && JsonNode.DeepEquals(actual, expected),
            $"contracts/control.action-vectors.json section '{sectionName}' is "
            + (actual is null ? "missing" : "stale")
            + $". Regenerate with {ControlActionVectorExportTests.RewriteSwitch}=1, or paste this section:\n"
            + expected.ToJsonString(Options));
    }
}
