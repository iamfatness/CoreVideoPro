using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Runs <c>contracts/tiles.fixtures.json</c> through the Windows production path
/// (FromPersisted -> TilesLayerPayloadBuilder.Build -> TilesLayerWire.From ->
/// MediaCoreCommandBuilder.BuildSceneGraphCommand). Windows is the oracle: the Mac
/// shell's Swift builder is checked against the same file
/// (ShellTestsTiles testEveryTilesFixtureMatchesTheSwiftBuilder). If this disagrees,
/// the fixture's <c>expected</c> is wrong, never this code.
/// </summary>
public class TilesFixtureTests
{
    private static readonly JsonSerializerOptions PersistedOptions = new()
    {
        PropertyNameCaseInsensitive = true
    };

    [Fact]
    public void EveryCaseMatchesTheWindowsBuilder()
    {
        var root = FindRepoRoot();
        Assert.NotNull(root);
        var doc = JsonNode.Parse(File.ReadAllText(Path.Combine(root!, "contracts", "tiles.fixtures.json")))!.AsObject();
        var checkedCases = 0;
        foreach (var node in doc["cases"]!.AsArray())
        {
            var fixture = node!.AsObject();
            var name = fixture["name"]!.GetValue<string>();
            var sceneId = fixture["sceneId"]!.GetValue<string>();
            var persisted = fixture["settings"].Deserialize<PersistedDynamicGallerySettings>(PersistedOptions)!;
            var scene = new Scene
            {
                Id = sceneId,
                Name = name,
                Layout = "dynamic-gallery",
                DynamicGallery = ScenePersistenceService.FromPersisted(persisted)
            };
            var participants = fixture["participants"]!.AsArray().Select(p => new Participant
            {
                Id = p!["id"]!.GetValue<string>(),
                Name = p["name"]!.GetValue<string>(),
                Health = p["videoOn"]!.GetValue<bool>() ? FeedHealth.Live : FeedHealth.VideoOff,
                PersistentId = p["persistentId"]?.GetValue<string>()
            }).ToList();
            var slots = fixture["slots"]!.AsArray().Select(ToSlot).ToList();
            var meetingId = fixture["meetingId"]?.GetValue<string>();
            var local = TilesLocalSourcePolicy.LocalVideoSources(slots, []).Select(s => s.Id).ToList();

            var payload = TilesLayerPayloadBuilder.Build(scene, participants,
                TilesLocalSourcePolicy.RoutedSourceIds(slots), participants, meetingId, local);
            var command = MediaCoreCommandBuilder.BuildSceneGraphCommand(sceneId, [], null, TilesLayerWire.From(payload));
            var actual = JsonNode.Parse(command.ExtensionData!["tiles"].GetRawText());

            AssertJsonEqual(fixture["expected"], actual, name + ": tiles");
            checkedCases++;
        }

        Assert.Equal(5, checkedCases);
    }

    private static ShowInputSlot ToSlot(JsonNode? node)
    {
        var kind = Enum.Parse<ShowInputKind>(node!["windowsKind"]!.GetValue<string>());
        var sourceId = node["sourceId"]!.GetValue<string>();
        return new ShowInputSlot
        {
            SlotNumber = node["slot"]!.GetValue<int>(),
            Kind = kind,
            // A Media slot stores its "media:<assetId>" id in ParticipantId (ShowInputRosterService.SlotSourceId).
            ParticipantId = kind switch
            {
                ShowInputKind.ZoomParticipant => sourceId,
                ShowInputKind.Media => "media:" + sourceId,
                _ => null
            },
            CaptureDeviceId = kind is ShowInputKind.ZoomParticipant or ShowInputKind.Media or ShowInputKind.Unassigned ? null : sourceId,
            InShow = node["inShow"]!.GetValue<bool>()
        };
    }

    /// Key sets equal; numbers equal within 1e-9 (int vs double is not a difference).
    private static void AssertJsonEqual(JsonNode? expected, JsonNode? actual, string path)
    {
        if (expected is null || actual is null)
        {
            Assert.True(expected is null && actual is null, $"{path}: expected {expected?.ToJsonString() ?? "null"}, got {actual?.ToJsonString() ?? "null"}");
            return;
        }

        switch (expected)
        {
            case JsonObject eo:
                var ao = Assert.IsType<JsonObject>(actual);
                var expectedKeys = string.Join(",", eo.Select(p => p.Key).OrderBy(k => k, StringComparer.Ordinal));
                var actualKeys = string.Join(",", ao.Select(p => p.Key).OrderBy(k => k, StringComparer.Ordinal));
                Assert.True(expectedKeys == actualKeys, $"{path}: keys expected [{expectedKeys}], got [{actualKeys}]");
                foreach (var (key, value) in eo)
                {
                    AssertJsonEqual(value, ao[key], $"{path}.{key}");
                }
                break;
            case JsonArray ea:
                var aa = Assert.IsType<JsonArray>(actual);
                Assert.True(ea.Count == aa.Count, $"{path}: expected {ea.ToJsonString()}, got {aa.ToJsonString()}");
                for (var i = 0; i < ea.Count; i++)
                {
                    AssertJsonEqual(ea[i], aa[i], $"{path}[{i}]");
                }
                break;
            default:
                var ev = expected.AsValue();
                var av = actual.AsValue();
                if (ev.GetValueKind() == JsonValueKind.Number)
                {
                    Assert.True(av.GetValueKind() == JsonValueKind.Number, $"{path}: expected a number, got {av.ToJsonString()}");
                    Assert.True(Math.Abs(ev.GetValue<double>() - av.GetValue<double>()) <= 1e-9,
                        $"{path}: expected {ev.ToJsonString()}, got {av.ToJsonString()}");
                }
                else
                {
                    Assert.True(ev.ToJsonString() == av.ToJsonString(), $"{path}: expected {ev.ToJsonString()}, got {av.ToJsonString()}");
                }
                break;
        }
    }

    private static string? FindRepoRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "contracts", "tiles.fixtures.json")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        return null;
    }
}
