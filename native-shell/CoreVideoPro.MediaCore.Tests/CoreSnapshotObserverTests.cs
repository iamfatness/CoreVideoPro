using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// Pins the read-only core-snapshot observation seam: the shell retains the core's OWN session
/// state text alongside the typed snapshot, and redacts it exactly once, at the boundary where it
/// may leave the process. The redaction assertions are this file's twin of
/// <c>SupportBundleExportTests</c>'s — a bundle and a served snapshot carry the same class of data
/// and must be held to the same rule.
/// </summary>
public sealed class CoreSnapshotObserverTests
{
    private static NativeMediaCoreStateSnapshot Parse(string state)
    {
        using var response = JsonDocument.Parse($"{{\"id\":\"core-1\",\"ok\":true,\"state\":{state}}}");
        var snapshot = CoreProtocolParser.TryParseSyncSnapshot(response);
        Assert.NotNull(snapshot);
        return snapshot!;
    }

    [Fact]
    public void ParsingRetainsTheNodesTheTypedSnapshotDoesNotBind()
    {
        // encoderEvidence / realtimeEvidence / tiles have no property on
        // NativeMediaCoreStateSnapshot: before the raw text was retained they were parsed and
        // discarded, and nothing outside the core could ever see them.
        var snapshot = Parse("""
            {"sceneId":"scene:solo",
             "encoderEvidence":{"queueDepth":4,"oldestQueuedAgeMs":31,"droppedVideo":0},
             "realtimeEvidence":{"render":{"deadlineMisses":2,"progressAgeMs":11.5}},
             "tiles":{"layerId":"wall","members":[{"sourceId":"zoom:1",
                      "rect":{"x":0,"y":0,"width":960,"height":540}}]}}
            """);

        Assert.Equal("scene:solo", snapshot.SceneId);
        Assert.NotNull(snapshot.RawJson);
        Assert.NotNull(snapshot.RawReceivedUtc);

        using var raw = JsonDocument.Parse(CoreSnapshotObserver.Observe(snapshot).Json!);
        Assert.Equal(4, raw.RootElement.GetProperty("encoderEvidence").GetProperty("queueDepth").GetInt32());
        Assert.Equal(2, raw.RootElement.GetProperty("realtimeEvidence").GetProperty("render")
            .GetProperty("deadlineMisses").GetInt32());
        Assert.Equal(960, raw.RootElement.GetProperty("tiles").GetProperty("members")[0]
            .GetProperty("rect").GetProperty("width").GetInt32());
    }

    [Fact]
    public void TheCoresStateJsonIsRecoverableFromEitherResponseShape()
    {
        // A real native core answers a sync with a WIRE state, which the supervisor maps onto a
        // synthesized base — a projection by construction. It tags the mapped record using this,
        // so the live path carries the core's own document too. Both wrapper keys must work.
        using var stateShaped = JsonDocument.Parse("""{"ok":true,"state":{"sceneId":"a","encoderEvidence":{"queueDepth":1}}}""");
        using var snapshotShaped = JsonDocument.Parse("""{"ok":true,"snapshot":{"sceneId":"b","encoderEvidence":{"queueDepth":2}}}""");
        using var neither = JsonDocument.Parse("""{"ok":true}""");

        Assert.Contains("encoderEvidence", CoreProtocolParser.TryGetStateJson(stateShaped)!);
        Assert.Contains("encoderEvidence", CoreProtocolParser.TryGetStateJson(snapshotShaped)!);
        Assert.Null(CoreProtocolParser.TryGetStateJson(neither));
    }

    [Fact]
    public void TheRawTextNeverLeavesThroughOrdinarySerialization()
    {
        // [JsonIgnore] is the belt to the redactor's braces: anything that serializes a snapshot
        // (support bundle, crash report, control state) must not pick the raw document up.
        var snapshot = Parse("""{"sceneId":"scene:solo","encoderEvidence":{"queueDepth":4}}""");
        var serialized = JsonSerializer.Serialize(snapshot,
            new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase });

        Assert.DoesNotContain("rawJson", serialized, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("rawReceivedUtc", serialized, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ObservationRedactsSecretsBeforeTheSnapshotCanLeaveTheProcess()
    {
        // Structured secret-shaped fields, and — the real risk — the same secret merely QUOTED in
        // an adapter's free text (an ffmpeg failure prints the rtmp URL with the key appended).
        var snapshot = Parse("""
            {"sceneId":"scene:solo",
             "outputSenderSession":{"status":"live","senders":[
               {"senderId":"rtmp-0","destination":"rtmp","status":"failed",
                "streamKey":"live_1234_SUPERSECRETKEY",
                "lastError":"ffmpeg exited: rtmps://live.example.com/app/live_1234_SUPERSECRETKEY",
                "runtimeDetail":"srtPassphrase=hunter2hunter2 accepted"}]},
             "browserSources":{"sources":[
               {"id":"b1","url":"https://scores.example.com/board?access_token=abc123def456"}]},
             "warnings":["configure-outputs echoed {\"streamKey\":\"live_1234_SUPERSECRETKEY\"}"]}
            """);

        var json = CoreSnapshotObserver.Observe(snapshot).Json!;

        Assert.DoesNotContain("SUPERSECRETKEY", json);
        Assert.DoesNotContain("hunter2hunter2", json);
        Assert.DoesNotContain("abc123def456", json);
        Assert.Contains("[redacted]", json);

        // Redaction must not cost the diagnosis: everything that is NOT a secret survives.
        using var raw = JsonDocument.Parse(json);
        var sender = raw.RootElement.GetProperty("outputSenderSession").GetProperty("senders")[0];
        Assert.Equal("rtmp-0", sender.GetProperty("senderId").GetString());
        Assert.Equal("failed", sender.GetProperty("status").GetString());
        Assert.Contains("ffmpeg exited", sender.GetProperty("lastError").GetString());
        Assert.Contains("live.example.com", sender.GetProperty("lastError").GetString());

        // Redacting a URL inside one string must not eat the properties after it. A text filter
        // run over the whole document does exactly that (its rtmp rule is greedy over
        // non-whitespace), which is why redaction walks the tree instead.
        Assert.Equal("[redacted]", sender.GetProperty("streamKey").GetString());
        Assert.Contains("accepted", sender.GetProperty("runtimeDetail").GetString());
        Assert.Equal("scene:solo", raw.RootElement.GetProperty("sceneId").GetString());
    }

    [Theory]
    // Overlay diagnostics whose names merely CONTAIN "key" must survive — losing keyPhase would
    // cost exactly the lower-third diagnosis this endpoint exists to enable.
    [InlineData("keyPhase", false)]
    [InlineData("keyer", false)]
    [InlineData("keyPosition", false)]
    [InlineData("keyLength", false)]
    [InlineData("layerId", false)]
    [InlineData("streamKey", true)]
    [InlineData("passphrase", true)]
    [InlineData("srtPassphrase", true)]
    [InlineData("accessToken", true)]
    [InlineData("sdkJwt", true)]
    [InlineData("userZak", true)]
    [InlineData("clientSecret", true)]
    [InlineData("pwd", true)]
    public void SecretNamedValuesAreDroppedWithoutTakingDiagnosticsWithThem(string name, bool secret)
        => Assert.Equal(secret, CoreSnapshotObserver.IsSecretName(name));

    [Fact]
    public void RecordingPathsAreDeliberatelyPreserved()
    {
        // Consistent with the support bundle's documented position ("ISO paths are not secrets"):
        // which file a take actually wrote to is load-bearing evidence, so it is kept. Pinned so
        // a future redaction change is a deliberate decision, not a silent one.
        var snapshot = Parse("""
            {"sceneId":"scene:solo",
             "recording":{"sessionId":"s1","status":"recording","writerStatus":"writing",
                          "targetFolder":"D:/Shows/2026-09-09","filenamePrefix":"show",
                          "format":"mp4","quality":"high",
                          "programPath":"D:/Shows/2026-09-09/show-program-0.mp4",
                          "streams":[{"kind":"program","status":"writing",
                                      "path":"D:/Shows/2026-09-09/show-program-0.mp4"}]}}
            """);

        Assert.Contains("show-program-0.mp4", CoreSnapshotObserver.Observe(snapshot).Json!);
    }

    [Fact]
    public void AbsentOrSynthesizedSnapshotsAreReportedAsUnavailable()
    {
        Assert.Null(CoreSnapshotObserver.Observe(null).Json);
        Assert.NotNull(CoreSnapshotObserver.Observe(null).UnavailableReason);

        // A snapshot the shell built itself (a generation fence) has no core text. Serving a
        // re-serialization of the typed record would be the hand-picked projection this seam
        // exists to replace, so it reports unavailable instead.
        var synthesized = new NativeMediaCoreStateSnapshot { SceneId = "scene:solo" };
        Assert.Null(CoreSnapshotObserver.Observe(synthesized).Json);
        Assert.NotNull(CoreSnapshotObserver.Observe(synthesized).UnavailableReason);
    }

    [Fact]
    public void ShellMergesCarryTheOriginalCoreTextAndItsReceiptTimeForward()
    {
        // Zoom capture / spine merges republish the snapshot on their own cadence. The core
        // evidence they carry is the tick it was parsed from, and the receipt timestamp is what
        // tells a consumer that.
        var snapshot = Parse("""{"sceneId":"scene:solo","encoderEvidence":{"queueDepth":4}}""");
        var merged = snapshot with { MeetingState = "in_meeting" };

        Assert.Equal(snapshot.RawJson, merged.RawJson);
        Assert.Equal(snapshot.RawReceivedUtc, merged.RawReceivedUtc);
    }
}
