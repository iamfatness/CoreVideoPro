using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// Writes (or checks) <c>contracts/telemetry.fixtures.json</c> from the REAL Windows telemetry code,
/// so the macOS port (mac-shell TelemetryPayload.swift / TelemetryConsent.swift) is held to Windows'
/// actual output, not to retyped values. Plan:
/// docs/superpowers/plans/2026-11-22-mac-control-telemetry-leftovers-phase4.md, Task 7.
/// <c>COREVIDEO_REWRITE_TELEMETRY_FIXTURES=1</c> rewrites the file.
/// </summary>
public sealed class TelemetryFixtureExportTests
{
    private const string StreamKey = "live_SUPERSECRETSTREAMKEY_9f3a2b";
    private const string RtmpUrl = "rtmp://a.rtmp.youtube.com/live2/" + StreamKey;
    private const string SrtPassphrase = "srt-passphrase-do-not-leak-42";
    private const string RecordingFolder = "/Users/producer/Movies/Confidential Client Show";
    private const string GuestName = "Alice Q. Attendee";
    private const string MeetingUrl = "https://zoom.us/j/98765432100?pwd=secretmeetingpassword";

    /// <summary>
    /// The secret-laden snapshot of TelemetryPayloadBuilderTests, as the core's OWN sync response.
    /// Fixture invariants: captureDevices has 4 rows (a duplicate id and a "detected" device) but
    /// only 2 distinct connected ids; one stream sender is live; one ISO id; two participants.
    /// </summary>
    private static JsonObject SecretLadenState() => new()
    {
        ["health"] = null,
        ["profile"] = null,
        ["meetingState"] = MeetingUrl,
        ["recording"] = new JsonObject
        {
            ["sessionId"] = "rec-1", ["active"] = true, ["status"] = "recording", ["writerStatus"] = "writing",
            ["targetFolder"] = RecordingFolder, ["filenamePrefix"] = "Show",
            ["format"] = "mp4", ["quality"] = "high", ["programPath"] = RecordingFolder + "/Program.mp4",
        },
        ["outputSenderSession"] = new JsonObject
        {
            ["status"] = "live", ["activeSenderCount"] = 1,
            ["senders"] = new JsonArray(new JsonObject
            {
                ["senderId"] = "rtmp-1", ["destination"] = RtmpUrl, ["status"] = "live", ["warning"] = SrtPassphrase,
            }),
        },
        ["virtualCamera"] = new JsonObject { ["enabled"] = true, ["status"] = "live" },
        ["isoParticipantIds"] = new JsonArray("zoom:pid-secret-1"),
        ["captureDevices"] = new JsonArray(
            Device("cam-a", "connected"), Device("cam-b", "connected"), Device("cam-b", "connected"),
            Device("screen:0", "detected")),
        ["participants"] = new JsonArray(
            new JsonObject { ["userId"] = "pid-secret-1", ["displayName"] = GuestName },
            new JsonObject { ["userId"] = "pid-secret-2", ["displayName"] = "Bob Secret" }),
    };

    /// <summary>A core with one NDI sender live and nothing else: NDI is not streaming.</summary>
    private static JsonObject NdiOnlyState() => new()
    {
        ["health"] = null,
        ["profile"] = null,
        ["outputSenderSession"] = new JsonObject
        {
            ["status"] = "live", ["activeSenderCount"] = 1,
            ["senders"] = new JsonArray(new JsonObject { ["senderId"] = "ndi", ["destination"] = "ndi", ["status"] = "live" }),
        },
    };

    private static JsonObject Device(string id, string state) =>
        new() { ["id"] = id, ["name"] = id.ToUpperInvariant(), ["connectionState"] = state };

    private static TelemetryMachineClass Machine(string? gpuTier) => new()
    {
        Label = "mac-arm64-cpu12-ram36gb", CpuCores = 12, RamBand = "32-64GB", GpuTier = gpuTier,
    };

    /// <summary>The live parse path (MediaCoreSupervisor: TryParseWireState, then
    /// MapNativeWireStateToSnapshot); null input = engine off.</summary>
    private static NativeMediaCoreStateSnapshot? Parse(string? responseJson)
    {
        if (responseJson is null) return null;
        using var doc = JsonDocument.Parse(responseJson);
        var wire = CoreProtocolParser.TryParseWireState(doc);
        Assert.NotNull(wire);
        return NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 1, wire!);
    }

    internal static JsonObject Build()
    {
        var payloads = new JsonArray();
        foreach (var (name, state, gpu) in new (string, JsonObject?, string?)[]
                 {
                     ("secret-laden", SecretLadenState(), "apple-m3-pro"),
                     ("engine-off", null, null),
                     ("ndi-only", NdiOnlyState(), "apple-m3-pro"),
                 })
        {
            var response = state is null ? null : new JsonObject { ["ok"] = true, ["snapshot"] = state }.ToJsonString();
            var payload = TelemetryPayloadBuilder.Build(TelemetryEventNames.SessionEnd, "1.2.3", 3600, Parse(response), 2, Machine(gpu));
            payloads.Add(new JsonObject
            {
                ["name"] = name,
                ["response"] = response,
                ["version"] = "1.2.3",
                ["sessionLengthSeconds"] = 3600,
                ["crashCount"] = 2,
                ["machine"] = JsonNode.Parse(JsonSerializer.Serialize(Machine(gpu),
                    new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase })),
                ["wire"] = TelemetryPayloadBuilder.SerializeWire(payload),
                ["preview"] = TelemetryPayloadBuilder.SerializePreview(payload),
            });
        }

        var ramBands = new JsonArray();
        foreach (var gb in new[] { 0, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 256 })
            ramBands.Add(new JsonObject { ["gb"] = gb, ["band"] = MachineClassProbe.RamBand(gb) });

        var ramRounding = new JsonArray();
        foreach (var gib in new[] { 7.5, 8.5, 16.4999, 35.9 })
        {
            var bytes = (long)(gib * 1024 * 1024 * 1024);
            ramRounding.Add(new JsonObject { ["bytes"] = bytes, ["gb"] = (int)Math.Round(bytes / (1024.0 * 1024 * 1024)) });
        }

        var consentFiles = new JsonArray();
        var dir = Path.Combine(Path.GetTempPath(), "cvp-telemetry-fixture-" + Guid.NewGuid().ToString("N"));
        try
        {
            foreach (var (name, state) in new (string, TelemetryConsentState)[]
                     {
                         ("enabled-with-last-sent", new TelemetryConsentState { Enabled = true, LastSentUtc = new DateTimeOffset(2026, 11, 22, 10, 0, 0, TimeSpan.Zero) }),
                         ("default", new TelemetryConsentState()),
                     })
            {
                var path = Path.Combine(dir, name + ".json");
                new TelemetryConsentStore(path).Save(state);
                consentFiles.Add(new JsonObject { ["name"] = name, ["json"] = File.ReadAllText(path).Replace("\r\n", "\n") });
            }
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
        }

        var config = new JsonArray();
        foreach (var (endpoint, key) in new (string?, string?)[]
                 {
                     (null, null), ("", "k"), ("  https://staging.example/  ", " k "), ("ftp://x", "k"), ("not a url", "k"),
                     (null, " "),
                 })
        {
            var resolved = CrashReportingConfig.Resolve(endpoint, key);
            config.Add(new JsonObject
            {
                ["endpointOverride"] = endpoint, ["apiKey"] = key,
                ["endpoint"] = resolved.Endpoint, ["enabled"] = resolved.Enabled, ["endpointHost"] = resolved.EndpointHost,
            });
        }

        return new JsonObject
        {
            ["version"] = 1,
            ["notes"] = "Written by native-shell/CoreVideoPro.MediaCore.Tests/TelemetryFixtureExportTests.cs from the Windows "
                + "telemetry code. payloads[].response is a core sync response parsed on the LIVE path "
                + "(CoreProtocolParser.TryParseWireState + NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot); "
                + "participants come from the core's own participants node there (no Zoom merger on this path). "
                + "Do not edit by hand.",
            ["payloads"] = payloads,
            ["ramBands"] = ramBands,
            ["ramRounding"] = ramRounding,
            ["consentFiles"] = consentFiles,
            ["config"] = config,
        };
    }

    [Fact]
    public void FixtureIsCurrent()
    {
        var expected = Build();
        var path = ControlRepoPaths.Contract("telemetry.fixtures.json");
        var options = new JsonSerializerOptions { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
        if (Environment.GetEnvironmentVariable("COREVIDEO_REWRITE_TELEMETRY_FIXTURES") == "1")
        {
            File.WriteAllText(path, expected.ToJsonString(options).Replace("\r\n", "\n") + "\n", new UTF8Encoding(false));
            return;
        }
        Assert.True(File.Exists(path), $"{path} is missing. Regenerate with COREVIDEO_REWRITE_TELEMETRY_FIXTURES=1.");
        var actual = JsonNode.Parse(File.ReadAllText(path));
        Assert.True(JsonNode.DeepEquals(actual, expected),
            "contracts/telemetry.fixtures.json is stale. Regenerate with COREVIDEO_REWRITE_TELEMETRY_FIXTURES=1.");
    }

    [Fact]
    public void TheSecretLadenPayloadCarriesNoSecretAndTheFixtureCounts()
    {
        // Fixture-derived operands (rule 2): the counts the fixture was built to produce.
        var wire = Build()["payloads"]!.AsArray()[0]!["wire"]!.GetValue<string>();
        foreach (var secret in new[] { StreamKey, SrtPassphrase, GuestName, "Confidential", "zoom.us", "pid-secret", "rtmp://" })
            Assert.DoesNotContain(secret, wire);
        using var doc = JsonDocument.Parse(wire);
        var shape = doc.RootElement.GetProperty("outputConfigShape");
        Assert.Equal(2, shape.GetProperty("captureSourceCount").GetInt32());
        Assert.Equal(2, shape.GetProperty("zoomParticipantCount").GetInt32());
        Assert.Equal(1, shape.GetProperty("isoSourceCount").GetInt32());
        Assert.True(shape.GetProperty("streamingEnabled").GetBoolean());
        Assert.True(shape.GetProperty("recordingEnabled").GetBoolean());
    }
}
