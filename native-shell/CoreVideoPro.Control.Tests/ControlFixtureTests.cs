using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using CoreVideoPro.Control.Osc;
using Xunit;

namespace CoreVideoPro.Control.Tests;

/// <summary>Runs contracts/control.fixtures.json through the Windows router. The Mac harness case
/// control/fixtures runs the same file through ControlRouter; editing a fixture's expectation must
/// turn both red. A missing file is a failure, never a skip.</summary>
public sealed class ControlFixtureTests
{
    private static JsonObject Fixtures() =>
        JsonNode.Parse(File.ReadAllText(RepoPaths.Contract("control.fixtures.json")))!.AsObject();

    private static bool ForWindows(JsonNode test) =>
        test["hosts"] is not JsonArray hosts || hosts.Any(h => h!.GetValue<string>() == "windows");

    private static JsonObject Case(string block, string name) =>
        Fixtures()[block]!.AsArray().Single(t => t!["name"]!.GetValue<string>() == name)!.AsObject();

    public static IEnumerable<object[]> HttpCases() =>
        Fixtures()["http"]!.AsArray().Where(t => ForWindows(t!)).Select(t => new object[] { t!["name"]!.GetValue<string>() });

    public static IEnumerable<object[]> SnapshotCases() =>
        Fixtures()["snapshot"]!.AsArray().Select(t => new object[] { t!["name"]!.GetValue<string>() });

    [Fact]
    public void EveryWindowsFixtureCaseIsRun()
    {
        // RC1 totals (each slice that appends cases updates these).
        Assert.Equal(35, HttpCases().Count());
        Assert.Equal(6, SnapshotCases().Count());
    }

    [Theory]
    [MemberData(nameof(HttpCases))]
    public async Task HttpCase(string name)
    {
        var test = Case("http", name);
        var request = test["request"]!.AsObject();
        var reply = test["surfaceReply"]?.AsObject();
        var surface = new ControlFixtureSurface
        {
            Reply = reply is null
                ? ControlInvokeResult.Success
                : new ControlInvokeResult(
                    reply["ok"]?.GetValue<bool>() ?? false,
                    reply["error"]?.GetValue<string>(),
                    reply["previewSceneId"]?.GetValue<string>(),
                    reply["data"]?.GetValue<string>())
        };
        var response = await new HttpControlRouter(surface).HandleAsync(
            request["method"]!.GetValue<string>(), request["path"]!.GetValue<string>(), request["body"]?.GetValue<string>());
        var expect = test["expect"]!.AsObject();
        Assert.Equal(expect["status"]!.GetValue<int>(), response.Status);
        var body = JsonNode.Parse(response.Body)!.AsObject();
        if (expect["errorPrefix"] is JsonNode prefix)
        {
            Assert.StartsWith(prefix.GetValue<string>(), body["error"]?.GetValue<string>() ?? "");
            body.Remove("error");
        }
        if (expect["body"] is JsonNode want)
        {
            Assert.True(JsonNode.DeepEquals(want, body), $"{name}: body {response.Body}");
        }
        if (expect["invocations"] is JsonArray invocations)
        {
            var got = new JsonArray(surface.Invocations
                .Select(i => (JsonNode)new JsonObject
                {
                    ["action"] = i.ActionId,
                    ["args"] = JsonSerializer.SerializeToNode(i.Args.ToArray())
                }).ToArray());
            Assert.True(JsonNode.DeepEquals(invocations, got), $"{name}: invocations {got.ToJsonString()}");
        }
    }

    [Theory]
    [MemberData(nameof(SnapshotCases))]
    public async Task SnapshotCase(string name)
    {
        var test = Case("snapshot", name);
        var observation = test["observation"]!.AsObject();
        IControlSurface surface;
        switch (observation["kind"]!.GetValue<string>())
        {
            case "available":
                var age = observation["ageMs"];
                surface = new ControlFixtureSurface
                {
                    // Windows' observer serves JSON already redacted (spec correction 2), so the
                    // state goes through as given.
                    Observation = new NativeSnapshotObservation(
                        observation["state"]!.ToJsonString(),
                        age is null ? null : DateTimeOffset.UtcNow - TimeSpan.FromMilliseconds(age.GetValue<double>()),
                        null)
                };
                break;
            case "unavailable":
                surface = new ControlFixtureSurface
                {
                    Observation = NativeSnapshotObservation.Unavailable(observation["reason"]!.GetValue<string>())
                };
                break;
            case "notObserved":
                surface = new FakeControlSurface();
                break;
            default:
                surface = new ControlFixtureSurface { Throw = observation["message"]!.GetValue<string>() };
                break;
        }
        var response = await new HttpControlRouter(surface).HandleAsync("GET", "/snapshot", null);
        Assert.Equal(200, response.Status);
        var root = JsonNode.Parse(response.Body)!.AsObject();
        foreach (var (key, want) in test["expect"]!.AsObject())
        {
            if (key == "ageMsNear")
            {
                var ageMs = root["ageMs"]?.GetValue<double>() ?? -1e9;
                Assert.True(Math.Abs(ageMs - want!.GetValue<double>()) <= 400, $"{name}: ageMs {ageMs} near {want}");
                continue;
            }
            Assert.True(root.TryGetPropertyValue(key, out var got), $"{name}: {key} is present");
            Assert.True(JsonNode.DeepEquals(want, got), $"{name}: {key} = {got?.ToJsonString() ?? "null"}");
        }
    }

    // ── OSC codec (osc.encode / osc.decode) ──

    public static IEnumerable<object[]> OscEncodeCases() =>
        Fixtures()["osc"]!["encode"]!.AsArray().Select(t => new object[] { t!["name"]!.GetValue<string>() });

    public static IEnumerable<object[]> OscDecodeCases() =>
        Fixtures()["osc"]!["decode"]!.AsArray().Select(t => new object[] { t!["name"]!.GetValue<string>() });

    private static JsonObject OscCase(string block, string name) =>
        Fixtures()["osc"]![block]!.AsArray().Single(t => t!["name"]!.GetValue<string>() == name)!.AsObject();

    private static object? OscArg(JsonNode arg) =>
        arg["i"] is JsonNode i ? i.GetValue<int>()
        : arg["f"] is JsonNode f ? (object)(float)f.GetValue<double>()
        : arg["s"] is JsonNode s ? s.GetValue<string>()
        : arg["bool"] is JsonNode b ? b.GetValue<bool>()
        : arg["blob"] is JsonNode blob ? Convert.FromHexString(blob.GetValue<string>())
        : null;

    [Fact]
    public void EveryOscCodecCaseIsRun()
    {
        Assert.Equal(7, OscEncodeCases().Count());
        Assert.Equal(12, OscDecodeCases().Count());
    }

    [Theory]
    [MemberData(nameof(OscEncodeCases))]
    public void OscEncodeCase(string name)
    {
        var test = OscCase("encode", name);
        var message = test["message"]!;
        var args = message["args"]!.AsArray().Select(a => OscArg(a!)).ToArray();
        var bytes = OscCodec.EncodeMessage(new OscMessage(message["address"]!.GetValue<string>(), args));
        Assert.Equal(test["hex"]!.GetValue<string>(), Convert.ToHexString(bytes).ToLowerInvariant());
    }

    [Theory]
    [MemberData(nameof(OscDecodeCases))]
    public void OscDecodeCase(string name)
    {
        var test = OscCase("decode", name);
        var decoded = OscCodec.Decode(Convert.FromHexString(test["hex"]!.GetValue<string>()));
        var want = test["messages"]!.AsArray();
        Assert.Equal(want.Count, decoded.Count);
        for (var m = 0; m < want.Count; m++)
        {
            Assert.Equal(want[m]!["address"]!.GetValue<string>(), decoded[m].Address);
            var wantArgs = want[m]!["args"]!.AsArray();
            Assert.Equal(wantArgs.Count, decoded[m].Args.Count);
            for (var a = 0; a < wantArgs.Count; a++)
            {
                var expected = OscArg(wantArgs[a]!);
                var got = decoded[m].Args[a];
                if (expected is byte[] bytes) Assert.Equal(bytes, Assert.IsType<byte[]>(got));
                else Assert.Equal(expected, got);
            }
        }
    }
}
