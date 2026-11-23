using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// <c>contracts/tiles-spine.fixtures.json</c>: each case's <c>expected</c> is PRODUCED by the real
/// <see cref="ZoomMediaSpinePayloadBuilder"/> (run with <c>COREVIDEO_REGENERATE_FIXTURES=1</c>),
/// never written by hand; otherwise this asserts the builder still produces it. The Mac shell's
/// Swift source-set port is checked against the same file (ShellTestsTiles,
/// testEverySpineFixtureMatchesTheSwiftSourceSet). Runs on macOS and Windows.
/// </summary>
public class TilesSpineFixtureTests
{
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    [Fact]
    public void EveryCaseMatchesTheWindowsBuilder()
    {
        var path = ControlRepoPaths.Contract("tiles-spine.fixtures.json");
        var doc = JsonNode.Parse(File.ReadAllText(path))!.AsObject();
        var regenerate = Environment.GetEnvironmentVariable("COREVIDEO_REGENERATE_FIXTURES") == "1";
        var cases = doc["cases"]!.AsArray();
        foreach (var node in cases)
        {
            var fixture = node!.AsObject();
            var name = fixture["name"]!.GetValue<string>();
            var actual = Project(ZoomMediaSpinePayloadBuilder.Build(ToBuildInput(fixture["input"]!.AsObject())));
            if (regenerate)
            {
                fixture["expected"] = actual;
                continue;
            }

            var expected = fixture["expected"];
            Assert.True(expected is not null, $"{name}: no expected (run with COREVIDEO_REGENERATE_FIXTURES=1)");
            Assert.True(JsonNode.DeepEquals(expected, actual),
                $"{name}: expected {expected!.ToJsonString()}\n got {actual.ToJsonString()}");
        }

        if (regenerate)
        {
            File.WriteAllText(path, doc.ToJsonString(Indented) + "\n");
        }

        Assert.True(cases.Count >= 7, $"expected at least 7 cases, found {cases.Count}");
    }

    private static ZoomMediaSpinePayloadBuilder.BuildInput ToBuildInput(JsonObject input)
    {
        var participants = input["participants"]!.AsArray().Select(p => new MediaCoreParticipantWire(
            p!["id"]!.GetValue<string>(), p["name"]!.GetValue<string>(), "guest", "main", "Main room",
            p["talking"]?.GetValue<bool>() ?? false, false, false,
            p["videoOn"]!.GetValue<bool>() ? "live" : "video-off")).ToList();
        return new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            Participants = participants,
            EngineRunning = true,
            OAuthSignedIn = true,
            StartCapture = true,
            ProgramSceneRoutes = Routes(input["programRoutes"]),
            PreviewSceneRoutes = Routes(input["previewRoutes"]),
            ProgramTilesLayer = Tiles(input["programTiles"], "tiles:program"),
            PreviewTilesLayer = Tiles(input["previewTiles"], "tiles:preview"),
            StickyAudioParticipantIds = Strings(input["sticky"]),
            IsoParticipantIds = Strings(input["iso"]),
            Multiview = input["multiview"] is JsonArray sources
                ? new MediaCoreMultiviewLayout(1920, 1080, 0, 0, sources.Select(s => new MediaCoreMultiviewSourceWire(
                    s!["sourceId"]?.GetValue<string>() ?? "zoom:" + s["participantId"]!.GetValue<string>(),
                    s["kind"]!.GetValue<string>(), s["slot"]!.GetValue<int>(), s["label"]!.GetValue<string>(),
                    s["participantId"]?.GetValue<string>())).ToList())
                : null
        };
    }

    private static IReadOnlyList<MediaCoreSceneRouteWire> Routes(JsonNode? node) =>
        node is JsonArray routes
            ? routes.Select(r => new MediaCoreSceneRouteWire(
                r!["routeId"]!.GetValue<string>(), r["mode"]!.GetValue<string>(), "mix",
                r["participantId"]?.GetValue<string>(),
                CaptureDeviceId: r["captureDeviceId"]?.GetValue<string>())).ToList()
            : [];

    private static MediaCoreTilesLayerWire? Tiles(JsonNode? node, string layerId) =>
        node is JsonObject tiles
            ? new MediaCoreTilesLayerWire(layerId, 0, Strings(tiles["members"]), "16:9", 16.0 / 9.0, 0.741, 0.741, "#000000",
                BackgroundSourceId: tiles["backgroundSourceId"]?.GetValue<string>() ?? string.Empty)
            : null;

    private static IReadOnlyList<string> Strings(JsonNode? node) =>
        node is JsonArray array ? array.Select(item => item!.GetValue<string>()).ToList() : [];

    /// The parts of the spine payload the Mac must reproduce.
    private static JsonObject Project(Dictionary<string, object?> payload)
    {
        var full = JsonNode.Parse(JsonSerializer.Serialize(payload))!.AsObject();
        var projected = new JsonObject
        {
            ["subscriptions"] = full["subscriptions"]!.DeepClone(),
            ["sourceParticipantIds"] = full["sourceParticipantIds"]!.DeepClone(),
            ["videoSubscriptionShortfall"] = full["videoSubscriptionShortfall"]!.DeepClone(),
            ["warnings"] = full["warnings"]!.DeepClone(),
        };
        if (full["multiview"] is JsonObject multiview)
        {
            projected["multiview"] = new JsonObject
            {
                ["programNotice"] = multiview["programNotice"]!.DeepClone(),
                ["previewNotice"] = multiview["previewNotice"]!.DeepClone(),
                ["labels"] = new JsonArray(multiview["sources"]!.AsArray()
                    .Select(source => (JsonNode?)JsonValue.Create(source!["label"]!.GetValue<string>())).ToArray()),
            };
        }

        return projected;
    }
}
