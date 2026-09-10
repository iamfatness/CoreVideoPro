using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using Xunit;

namespace CoreVideoPro.Control.Tests;

/// <summary>A control surface that also observes a media core, for GET /snapshot.</summary>
public sealed class FakeObservingControlSurface : IControlSurface, INativeSnapshotObserver
{
    public ControlState State { get; set; } = ControlState.Empty;

    public NativeSnapshotObservation Observation { get; set; } =
        NativeSnapshotObservation.Unavailable("no snapshot");

    public Func<NativeSnapshotObservation>? ObservationFactory { get; set; }

    public int ObservationReads { get; private set; }

    public Task<ControlInvokeResult> InvokeAsync(string actionId, IReadOnlyList<object?> args, CancellationToken cancellationToken = default)
        => Task.FromResult(ControlInvokeResult.Success);

    public ControlState GetState() => State;

    public NativeSnapshotObservation GetNativeSnapshot()
    {
        ObservationReads++;
        return ObservationFactory is not null ? ObservationFactory() : Observation;
    }

    public event EventHandler<ControlState>? StateChanged;

    public void RaiseStateChanged(ControlState state) => StateChanged?.Invoke(this, state);
}

public sealed class HttpSnapshotRouteTests
{
    // A trimmed but structurally real core session state: the nodes the shell's typed snapshot
    // model does NOT bind are exactly the ones a live show needs.
    private const string CoreSnapshot = """
    {
      "sceneId": "scene:solo",
      "programBuffer": { "underruns": 3, "overflows": 0, "gpuNotReady": 1, "occupancy": 4,
                         "delivered": 5100, "produced": 5104, "outputSequenceGaps": 0 },
      "encoderEvidence": { "metricVersion": "async-encoder-evidence-v1", "lifecycleState": "producing",
                           "queueDepth": 2, "oldestQueuedAgeMs": 18, "droppedVideo": 0,
                           "droppedAudio": 0, "programVideoWritten": 5099 },
      "realtimeEvidence": { "metricVersion": "realtime-worker-evidence-v1",
                            "render": { "observed": true, "progressAgeMs": 12.5, "completedSlots": 5100,
                                        "skippedSlots": 1, "deadlineMisses": 2 },
                            "audio": { "observed": true, "progressAgeMs": 4.0, "completedTicks": 9900 },
                            "videoOutput": { "observed": true, "progressAgeMs": 9.0 } },
      "tiles": { "layerId": "wall", "members": [ { "sourceId": "zoom:1",
                 "rect": { "x": 0, "y": 0, "width": 960, "height": 540 } } ] },
      "multiviewer": { "layoutMode": "pgmPvwTop", "tileCount": 8 },
      "browserSources": { "sources": [] }
    }
    """;

    private static JsonElement Get(HttpControlResponse response)
    {
        Assert.Equal(200, response.Status);
        return JsonDocument.Parse(response.Body).RootElement.Clone();
    }

    [Fact]
    public async Task SnapshotPassesEveryCoreNodeThroughVerbatim()
    {
        var received = DateTimeOffset.UtcNow.AddMilliseconds(-120);
        var surface = new FakeObservingControlSurface
        {
            Observation = new NativeSnapshotObservation(CoreSnapshot, received, null)
        };

        var root = Get(await new HttpControlRouter(surface).HandleAsync("GET", "/snapshot", null));

        Assert.True(root.GetProperty("available").GetBoolean());
        var nodes = root.GetProperty("nodes").EnumerateArray().Select(node => node.GetString()).ToArray();
        Assert.Contains("encoderEvidence", nodes);
        Assert.Contains("realtimeEvidence", nodes);
        Assert.Contains("programBuffer", nodes);
        Assert.Contains("tiles", nodes);
        Assert.Contains("multiviewer", nodes);

        // The payload is the core's own document, not a projection of it: values the shell's
        // ControlState has no field for must still be readable.
        var snapshot = root.GetProperty("snapshot");
        Assert.Equal(2, snapshot.GetProperty("encoderEvidence").GetProperty("queueDepth").GetInt32());
        Assert.Equal(2, snapshot.GetProperty("realtimeEvidence").GetProperty("render").GetProperty("deadlineMisses").GetInt32());
        Assert.Equal(3, snapshot.GetProperty("programBuffer").GetProperty("underruns").GetInt32());
        Assert.Equal(960, snapshot.GetProperty("tiles").GetProperty("members")[0]
            .GetProperty("rect").GetProperty("width").GetInt32());
    }

    [Fact]
    public async Task SnapshotReportsShellReceiptTimeSeparatelyFromRequestTime()
    {
        var received = DateTimeOffset.UtcNow.AddMilliseconds(-250);
        var surface = new FakeObservingControlSurface
        {
            Observation = new NativeSnapshotObservation(CoreSnapshot, received, null)
        };

        var root = Get(await new HttpControlRouter(surface).HandleAsync("GET", "/snapshot", null));

        Assert.Equal(received.ToUniversalTime(), root.GetProperty("receivedUtc").GetDateTimeOffset().ToUniversalTime(),
            TimeSpan.FromMilliseconds(2));
        Assert.True(root.GetProperty("servedUtc").GetDateTimeOffset() >= received);
        Assert.True(root.GetProperty("ageMs").GetDouble() >= 250);
        Assert.False(root.GetProperty("stale").GetBoolean());
    }

    [Fact]
    public async Task AnOldSnapshotIsMarkedStaleRatherThanReadingAsCurrent()
    {
        var surface = new FakeObservingControlSurface
        {
            Observation = new NativeSnapshotObservation(
                CoreSnapshot,
                DateTimeOffset.UtcNow - HttpControlRouter.SnapshotStaleAfter - TimeSpan.FromSeconds(1),
                null)
        };

        var root = Get(await new HttpControlRouter(surface).HandleAsync("GET", "/snapshot", null));

        Assert.True(root.GetProperty("available").GetBoolean());
        Assert.True(root.GetProperty("stale").GetBoolean());
    }

    [Fact]
    public async Task AbsentSnapshotIsExplicitlyUnavailableNotAnEmptyObject()
    {
        var surface = new FakeObservingControlSurface
        {
            Observation = NativeSnapshotObservation.Unavailable("engine off")
        };

        var root = Get(await new HttpControlRouter(surface).HandleAsync("GET", "/snapshot", null));

        Assert.False(root.GetProperty("available").GetBoolean());
        Assert.True(root.GetProperty("stale").GetBoolean());
        Assert.Equal("no-snapshot", root.GetProperty("reasonCode").GetString());
        Assert.Equal("engine off", root.GetProperty("reason").GetString());
        Assert.Equal(JsonValueKind.Null, root.GetProperty("snapshot").ValueKind);
    }

    [Fact]
    public async Task ASurfaceWithoutACoreSaysSoInsteadOfFailing()
    {
        var root = Get(await new HttpControlRouter(new FakeControlSurface()).HandleAsync("GET", "/snapshot", null));

        Assert.False(root.GetProperty("available").GetBoolean());
        Assert.Equal("not-observed", root.GetProperty("reasonCode").GetString());
    }

    [Fact]
    public async Task AFailingOrMalformedObservationIsAnsweredNotThrown()
    {
        var throwing = new FakeObservingControlSurface
        {
            ObservationFactory = () => throw new InvalidOperationException("bridge gone")
        };
        var thrownRoot = Get(await new HttpControlRouter(throwing).HandleAsync("GET", "/snapshot", null));
        Assert.Equal("observer-failed", thrownRoot.GetProperty("reasonCode").GetString());

        var malformed = new FakeObservingControlSurface
        {
            Observation = new NativeSnapshotObservation("{not json", DateTimeOffset.UtcNow, null)
        };
        var malformedRoot = Get(await new HttpControlRouter(malformed).HandleAsync("GET", "/snapshot", null));
        Assert.Equal("unparseable", malformedRoot.GetProperty("reasonCode").GetString());
    }

    [Fact]
    public async Task ServingASnapshotReadsShellStateOnceWithNoCoreRoundTrip()
    {
        var surface = new FakeObservingControlSurface
        {
            Observation = new NativeSnapshotObservation(CoreSnapshot, DateTimeOffset.UtcNow, null)
        };
        var router = new HttpControlRouter(surface);

        await router.HandleAsync("GET", "/state", null);
        Assert.Equal(0, surface.ObservationReads);

        await router.HandleAsync("GET", "/snapshot", null);
        Assert.Equal(1, surface.ObservationReads);
    }

    [Fact]
    public async Task SnapshotIsBehindTheSameTokenAsEveryOtherRoute()
    {
        var surface = new FakeObservingControlSurface
        {
            Observation = new NativeSnapshotObservation(CoreSnapshot, DateTimeOffset.UtcNow, null)
        };
        var port = GetFreePort();
        await using var server = new HttpControlServer(surface,
            new HttpControlServerOptions { ListenPort = port, AuthToken = "s3cret" });
        server.Start();

        using var http = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}/") };
        using var denied = await http.GetAsync("snapshot");
        Assert.Equal(HttpStatusCode.Unauthorized, denied.StatusCode);
        Assert.DoesNotContain("realtimeEvidence", await denied.Content.ReadAsStringAsync());

        http.DefaultRequestHeaders.Authorization = new("Bearer", "s3cret");
        using var allowed = await http.GetAsync("snapshot");
        Assert.Equal(HttpStatusCode.OK, allowed.StatusCode);
        Assert.Contains("realtimeEvidence", await allowed.Content.ReadAsStringAsync());
    }

    [Fact]
    public void ALanBindStillRefusesToServeSnapshotsWithoutAToken()
    {
        // The snapshot route adds no new unauthenticated LAN surface: the whole listener refuses
        // to start on a non-loopback bind with no token, so there is nothing to reach.
        Assert.Throws<InvalidOperationException>(() =>
            new HttpControlServerOptions { Host = "+", ListenPort = 8011 }.Validate());
        new HttpControlServerOptions { Host = "+", ListenPort = 8011, AuthToken = "s3cret" }.Validate();
    }

    private static int GetFreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }
}
