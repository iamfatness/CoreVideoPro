using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// The MediaCore-side writer of <c>contracts/control.action-vectors.json</c> (plan
/// docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md, Task 3). Section
/// <c>multiview-overlays</c>: the <c>configure-multiviewer</c> command the Windows builder emits for the
/// four overlay fixtures, recorded as <c>effects.command</c>. The plan named the WinUI tests as its
/// writer; the builder lives in MediaCore, which runs on macOS too, so this section regenerates locally
/// (<c>COREVIDEO_REWRITE_ACTION_VECTORS=1</c>) and is checked on every host that runs MediaCore.Tests.
/// </summary>
public sealed class ControlActionVectorExportTests
{
    /// <summary>Each case: the overlay state BEFORE the action (<c>given</c>), the action that moves it,
    /// and the state after. Fixture invariant: applying <c>Action(Arg)</c> to <c>Before</c> yields
    /// <c>After</c>, and the four <c>After</c> states are the plan's fixtures (all default; all off;
    /// labels off only; clock on only). Only <c>effects.command</c> is Windows OUTPUT (the real
    /// builder); the action-to-flag mapping is this fixture's ASSERTION of StudioControlSurface's
    /// dispatch (cs:679-690, one flag per action), which is not run here.</summary>
    private static readonly (string Name, bool[] Before, string Action, bool Arg, bool[] After)[] Cases =
    [
        ("all-default", [true, true, true, false], "multiview.showLabels.set", true, [true, true, true, false]),
        ("all-off", [false, true, false, false], "multiview.showTally.set", false, [false, false, false, false]),
        ("labels-off-only", [true, true, true, false], "multiview.showLabels.set", false, [false, true, true, false]),
        ("clock-on-only", [true, true, true, false], "multiview.showClock.set", true, [true, true, true, true]),
    ];

    private const string LayoutMode = "pgmPvwTop";
    private const int TileCount = 10;

    internal static JsonObject MultiviewOverlaysSection()
    {
        var cases = new JsonArray();
        foreach (var (name, before, action, arg, after) in Cases)
        {
            var command = MediaCoreCommandBuilder.BuildConfigureMultiviewerCommand(
                LayoutMode, TileCount, after[0], after[1], after[2], after[3]);
            cases.Add(new JsonObject
            {
                ["name"] = name,
                ["action"] = action,
                ["args"] = new JsonArray(arg),
                ["given"] = new JsonObject
                {
                    ["layoutMode"] = LayoutMode,
                    ["tileCount"] = TileCount,
                    ["showLabels"] = before[0],
                    ["showTally"] = before[1],
                    ["showMeters"] = before[2],
                    ["showClock"] = before[3],
                },
                ["expect"] = new JsonObject
                {
                    ["ok"] = true,
                    ["effects"] = new JsonObject { ["command"] = JsonNode.Parse(JsonSerializer.Serialize(command)) },
                },
            });
        }
        return new JsonObject
        {
            ["source"] = "native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs BuildConfigureMultiviewerCommand",
            ["writer"] = "MCT",
            ["cases"] = cases,
        };
    }

    [Fact]
    public void MultiviewOverlaysSectionIsCurrent()
    {
        var expected = MultiviewOverlaysSection();
        var path = ControlRepoPaths.Contract("control.action-vectors.json");
        var options = new JsonSerializerOptions { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
        var root = File.Exists(path)
            ? JsonNode.Parse(File.ReadAllText(path))!.AsObject()
            : new JsonObject { ["version"] = 1, ["sections"] = new JsonObject() };
        var sections = root["sections"]!.AsObject();
        if (Environment.GetEnvironmentVariable("COREVIDEO_REWRITE_ACTION_VECTORS") == "1")
        {
            sections["multiview-overlays"] = expected.DeepClone();
            File.WriteAllText(path, root.ToJsonString(options).Replace("\r\n", "\n") + "\n", new UTF8Encoding(false));
            return;
        }
        var actual = sections["multiview-overlays"];
        Assert.True(actual is not null && JsonNode.DeepEquals(actual, expected),
            "contracts/control.action-vectors.json section 'multiview-overlays' is "
            + (actual is null ? "missing" : "stale")
            + ". Regenerate with COREVIDEO_REWRITE_ACTION_VECTORS=1:\n" + expected.ToJsonString(options));
    }

    [Fact]
    public void TheCommandCarriesTheFourFlags()
    {
        // Fixture-derived operands: the clock-on fixture sends showClock true and keeps the rest default.
        var clockOn = MultiviewOverlaysSection()["cases"]!.AsArray()
            .Single(c => (string?)c!["name"] == "clock-on-only")!["expect"]!["effects"]!["command"]!;
        Assert.Equal("configure-multiviewer", (string?)clockOn["type"]);
        Assert.True((bool)clockOn["showClock"]!);
        Assert.True((bool)clockOn["showLabels"]!);
        Assert.True((bool)clockOn["showTally"]!);
        Assert.True((bool)clockOn["showMeters"]!);
        Assert.Equal(10, (int)clockOn["tileCount"]!);
    }
}
