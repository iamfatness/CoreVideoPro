using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// The WinUI-side writer of <c>contracts/control.action-vectors.json</c> (plan
/// docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md, Task 1 step 2). Each
/// section runs fixture inputs through the REAL Windows helper and records what it produced; the macOS
/// shell replays the section (mac-shell ControlApiTestsRC7.swift). This assembly runs only on the
/// Windows CI job, so a new or stale section is regenerated there: the failure message prints the
/// section JSON to paste into the file (or run with <c>COREVIDEO_REWRITE_ACTION_VECTORS=1</c> on a
/// Windows checkout). A section is never hand-typed.
/// </summary>
public sealed class ControlActionVectorExportTests
{
    private const string Srt = "transport.stream.srt.configure";
    private const string StoredPassphrase = "stored-passphrase-1";

    private static readonly object?[] ValidSrt =
        [true, "caller", "example.test", "9000", "120", "test-stream-id", "16", "test-passphrase-sentinel"];

    private static object?[] With(int index, object? value)
    {
        var args = ValidSrt.ToArray();
        args[index] = value;
        return args;
    }

    /// <summary>
    /// <c>srt-configure</c> inputs. Fixture invariants: <see cref="ValidSrt"/> passes every check (its
    /// passphrase is 24 characters, key length 16); each refusal case breaks exactly one field;
    /// <see cref="StoredPassphrase"/> is 10-79 characters so a kept passphrase still validates.
    /// </summary>
    private static readonly (string Name, object?[] Args, bool Busy, string Stored)[] SrtCases =
    [
        ("valid-caller-with-passphrase", ValidSrt, false, ""),
        ("busy-refuses-first", With(1, "invalid-mode-test-secret"), true, ""),
        ("schema-empty-args", [], false, ""),
        ("mode-invalid", With(1, "invalid-mode-test-secret"), false, ""),
        ("mode-trimmed-and-lowercased", With(1, " Listener "), false, ""),
        ("host-with-scheme", With(2, "srt://x"), false, ""),
        ("host-with-query", With(2, "h?x=1"), false, ""),
        ("host-ipv4", With(2, "192.168.1.20"), false, ""),
        ("host-ipv6-bracketed", With(2, "[::1]"), false, ""),
        ("port-zero", With(3, "0"), false, ""),
        ("port-65536", With(3, "65536"), false, ""),
        ("latency-19", With(4, "19"), false, ""),
        ("stream-id-513", With(5, new string('a', 513)), false, ""),
        ("key-length-8", With(6, "8"), false, ""),
        ("key-16-no-passphrase", ValidSrt[..7], false, ""),
        ("passphrase-9", With(7, "123456789"), false, ""),
        ("passphrase-80", With(7, new string('p', 80)), false, ""),
        ("key-0-with-passphrase", With(6, "0"), false, ""),
        ("omitted-passphrase-kept", ValidSrt[..7], false, StoredPassphrase),
        ("empty-passphrase-clears", [true, "caller", "example.test", "9000", "120", "", "0", ""], false, StoredPassphrase),
        ("disabled-empty-host-saves", [false, "caller", "", "9000", "120", "", "0"], false, ""),
    ];

    internal static JsonObject SrtConfigureSection()
    {
        var cases = new JsonArray();
        foreach (var (name, args, busy, stored) in SrtCases)
        {
            var current = new ProductionOutputPreferences { StreamSrtPassphrase = stored };
            var steps = new List<string>();
            ProductionOutputPreferences? saved = null;
            var result = StreamDestinationControl.Configure(true, args, busy, current,
                next => { steps.Add("save"); saved = next; },
                _ => steps.Add("apply"));
            var effects = new JsonObject
            {
                ["saved"] = steps.Contains("save"),
                ["applied"] = steps.Contains("apply"),
                ["steps"] = new JsonArray(steps.Select(s => (JsonNode?)s).ToArray()),
            };
            if (saved is not null)
            {
                effects["enabled"] = saved.StreamSrtEnabled;
                effects["mode"] = saved.StreamSrtMode;
                effects["host"] = saved.StreamSrtHost;
                effects["port"] = saved.StreamSrtPort;
                effects["latencyMs"] = saved.StreamSrtLatencyMs;
                effects["streamId"] = saved.StreamSrtStreamId;
                effects["keyLength"] = saved.StreamSrtKeyLength;
                var passphrase = saved.StreamSrtPassphrase ?? "";
                effects["passphrase"] = passphrase.Length == 0 ? "cleared"
                    : stored.Length > 0 && passphrase == stored ? "kept" : "set";
            }
            var expect = new JsonObject { ["ok"] = result.Ok, ["effects"] = effects };
            if (!result.Ok) expect["error"] = result.Error;
            cases.Add(new JsonObject
            {
                ["name"] = name,
                ["action"] = Srt,
                ["args"] = ToJson(args),
                ["given"] = new JsonObject { ["busy"] = busy, ["storedPassphrase"] = stored },
                ["expect"] = expect,
            });
        }
        return Section("native-shell/CoreVideoPro.WinUI/Services/StreamDestinationControl.cs:9", cases);
    }

    /// <summary><c>ndi-set</c>: the turn-ON validation (<c>SetNdiOutput</c> validates only on; off is
    /// never validated). Inputs follow plan Task 6.</summary>
    private static readonly (string Name, string? Program, string? Graphics, bool KeyOn, bool? CoreNdi, bool? CoreKey)[] NdiCases =
    [
        ("no-program-name", "", null, false, true, true),
        ("capability-missing", "Studio A", null, false, false, true),
        ("graphics-key-equals-program", "Studio A", "studio a", true, true, true),
        ("valid", "Studio A", null, true, true, true),
        ("core-not-up-defers", "Studio A", null, false, null, null),
    ];

    internal static JsonObject NdiSetSection()
    {
        var cases = new JsonArray();
        foreach (var (name, program, graphics, keyOn, coreNdi, coreKey) in NdiCases)
        {
            var error = StudioStreamOutputValidation.ValidateNdiOutput(program, graphics, keyOn, coreNdi, coreKey);
            var expect = new JsonObject { ["ok"] = error is null };
            if (error is not null) expect["error"] = error;
            cases.Add(new JsonObject
            {
                ["name"] = name,
                ["action"] = "transport.ndi.set",
                ["args"] = new JsonArray(true),
                ["given"] = new JsonObject
                {
                    ["programName"] = program,
                    ["graphicsName"] = graphics,
                    ["graphicsKeyOn"] = keyOn,
                    ["coreHasNdiOutput"] = coreNdi,
                    ["coreHasGraphicsKey"] = coreKey,
                },
                ["expect"] = expect,
            });
        }
        return Section("native-shell/CoreVideoPro.WinUI/Services/StudioStreamOutputValidation.cs:262", cases);
    }

    /// <summary><c>restore-starters</c>: the status sentence for 0, 1 and 3 restored scenes.</summary>
    internal static JsonObject RestoreStartersSection()
    {
        var cases = new JsonArray();
        foreach (var (name, restored) in new (string, string[])[]
                 {
                     ("none-missing", []),
                     ("one-missing", ["Two-up"]),
                     ("three-missing", ["Two-up", "Solo", "Side by side"]),
                 })
        {
            cases.Add(new JsonObject
            {
                ["name"] = name,
                ["action"] = "scene.restoreStarters",
                ["args"] = new JsonArray(),
                ["given"] = new JsonObject { ["restoredNames"] = new JsonArray(restored.Select(n => (JsonNode?)n).ToArray()) },
                ["expect"] = new JsonObject
                {
                    ["ok"] = true,
                    ["effects"] = new JsonObject { ["status"] = SceneCatalogPolicy.RestoreStatus(restored) },
                },
            });
        }
        return Section("native-shell/CoreVideoPro.WinUI/Services/SceneCatalogPolicy.cs RestoreStatus", cases);
    }

    [Fact]
    public void SrtConfigureSectionIsCurrent() => Check("srt-configure", SrtConfigureSection());

    [Fact]
    public void NdiSetSectionIsCurrent() => Check("ndi-set", NdiSetSection());

    [Fact]
    public void RestoreStartersSectionIsCurrent() => Check("restore-starters", RestoreStartersSection());

    [Fact]
    public void SrtVectorsNeverEchoArgumentValues()
    {
        // Fixture-derived operands: the two sentinels the args carry must never reach a reply.
        foreach (var node in SrtConfigureSection()["cases"]!.AsArray())
        {
            var error = node!["expect"]!["error"]?.GetValue<string>() ?? "";
            Assert.DoesNotContain("invalid-mode-test-secret", error);
            Assert.DoesNotContain("test-passphrase-sentinel", error);
        }
    }

    private static JsonObject Section(string source, JsonArray cases) =>
        new() { ["source"] = source, ["writer"] = "WT", ["cases"] = cases };

    private static JsonArray ToJson(object?[] args) =>
        new(args.Select(arg => arg switch
        {
            null => null,
            bool b => (JsonNode?)JsonValue.Create(b),
            string s => JsonValue.Create(s),
            _ => throw new InvalidOperationException($"Unsupported fixture arg {arg.GetType()}"),
        }).ToArray());

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    /// <summary>Same contract as CoreVideoPro.Control.Tests' <c>ActionVectorFile</c> (duplicated: the
    /// test assemblies share no code). Rewrites this section only and keeps every other writer's.</summary>
    private static void Check(string sectionName, JsonObject expected)
    {
        var path = FindVectorFile();
        var root = File.Exists(path)
            ? JsonNode.Parse(File.ReadAllText(path))!.AsObject()
            : new JsonObject { ["version"] = 1, ["sections"] = new JsonObject() };
        var sections = root["sections"]!.AsObject();
        if (Environment.GetEnvironmentVariable("COREVIDEO_REWRITE_ACTION_VECTORS") == "1")
        {
            sections[sectionName] = expected.DeepClone();
            File.WriteAllText(path, root.ToJsonString(Options).Replace("\r\n", "\n") + "\n", new UTF8Encoding(false));
            return;
        }
        var actual = sections[sectionName];
        Assert.True(actual is not null && JsonNode.DeepEquals(actual, expected),
            $"contracts/control.action-vectors.json section '{sectionName}' is "
            + (actual is null ? "missing" : "stale")
            + ". Paste this section (or rewrite with COREVIDEO_REWRITE_ACTION_VECTORS=1):\n"
            + "ACTION-VECTOR-SECTION-BEGIN " + sectionName + "\n"
            + expected.ToJsonString(Options) + "\nACTION-VECTOR-SECTION-END " + sectionName);
    }

    private static string FindVectorFile()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "contracts", "README.md")))
                return Path.Combine(dir.FullName, "contracts", "control.action-vectors.json");
        }
        throw new InvalidOperationException("Repository root (contracts/README.md) not found.");
    }
}
