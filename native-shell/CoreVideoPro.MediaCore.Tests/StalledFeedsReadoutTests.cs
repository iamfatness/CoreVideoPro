using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>#1074: the "network congested" status line. The live show of 2026-10-31 read as
/// participants dropping; the snapshot showed the RTMP queue in backpressure and three or four
/// Zoom feeds stalled at the same second. These pin when the line appears and what it says.</summary>
public sealed class NetworkCongestionReadoutTests
{
    private static NativeMediaCoreSourceHealth Source(string id, string kind, string health, bool hasVideo = true) =>
        new() { SourceId = id, Kind = kind, Health = health, HasVideo = hasVideo };

    private static NativeMediaCoreStateSnapshot Snapshot(
        NativeMediaCoreSenderBackpressure? pressure,
        params NativeMediaCoreSourceHealth[] sources) => new()
    {
        OutputProfile = new() { ProfileId = "test", Resolution = "1920x1080", Fps = 60 },
        OutputSenderSession = new()
        {
            Status = "live",
            Senders = [new() { SenderId = "rtmp", Destination = "rtmp", Status = "live", Backpressure = pressure }]
        },
        Sources = sources
    };

    private static readonly NativeMediaCoreSenderBackpressure Level1Queue445 =
        new() { Divisor = 2, AppliedDivisor = 1, Level = 1, BufferedMs = 445 };

    [Fact]
    public void TheLiveShowShapeProducesTheLine()
    {
        // 09:59:33 EDT: `[stream-backpressure] enter divisor=2 buffered=445ms`, three Zoom
        // participants stalled on the same one-second sample. appliedDivisor stays 1 since #538.
        var snapshot = Snapshot(Level1Queue445,
            Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled"),
            Source("zoom:3", "zoom", "stalled"), Source("zoom:4", "zoom", "producing"));

        Assert.Equal("network congested: stream 445 ms behind, 3 feeds stalled", NetworkCongestionReadout.Format(snapshot));
    }

    [Fact]
    public void AnAppliedDivisorIsReportedAsARate()
    {
        var pressure = new NativeMediaCoreSenderBackpressure { Divisor = 3, AppliedDivisor = 3, Level = 2, BufferedMs = 900 };
        var snapshot = Snapshot(pressure, Source("zoom:1", "zoom", "stalled"), Source("capture:cam", "capture", "stalled"));

        Assert.Equal("network congested: stream at 1/3 rate, 2 feeds stalled", NetworkCongestionReadout.Format(snapshot));
    }

    [Fact]
    public void ADrainedQueueStillUnderPressureReportsTheLevelNotAnInventedDepth()
    {
        var pressure = new NativeMediaCoreSenderBackpressure { Divisor = 2, AppliedDivisor = 1, Level = 1, BufferedMs = 0 };
        var snapshot = Snapshot(pressure, Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled"));

        Assert.Equal("network congested: stream backpressure level 1, 2 feeds stalled", NetworkCongestionReadout.Format(snapshot));
    }

    [Fact]
    public void OneStalledFeedIsThatFeedsProblemNotTheLinks()
    {
        var snapshot = Snapshot(Level1Queue445, Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "producing"));
        Assert.Null(NetworkCongestionReadout.Format(snapshot));
    }

    [Fact]
    public void StalledFeedsWithoutStreamBackpressureAreNotCalledCongestion()
    {
        // 09:54:51, before the stream started: all four stalled, no pressured sender. The Tiles
        // hold covers the picture; this line stays quiet because the stream fact is absent.
        var healthy = new NativeMediaCoreSenderBackpressure { Divisor = 1, AppliedDivisor = 1, Level = 0 };
        Assert.Null(NetworkCongestionReadout.Format(Snapshot(healthy,
            Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled"))));
        Assert.Null(NetworkCongestionReadout.Format(Snapshot(null,
            Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled"))));
    }

    [Theory]
    [InlineData("starting")]
    [InlineData("failed")]
    [InlineData("idle")]
    public void ANonProducingSenderDoesNotCountAsBackpressure(string status)
    {
        var snapshot = Snapshot(Level1Queue445, Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled")) with
        {
            OutputSenderSession = new()
            {
                Status = status,
                Senders = [new() { SenderId = "rtmp", Destination = "rtmp", Status = status, Backpressure = Level1Queue445 }]
            }
        };
        Assert.Null(NetworkCongestionReadout.Format(snapshot));
    }

    [Fact]
    public void StillsClipsAudioOnlyAndComposedSourcesAreNotFeeds()
    {
        // The bus calls anything 200 ms past its last new frame "stalled", which a paused clip, a
        // still or the Tiles wall itself does while healthy. Only zoom and capture video count.
        var sources = new[]
        {
            Source("still:logo", "still", "stalled"),
            Source("media:clip", "media", "stalled"),
            Source("tiles:s", "composed", "stalled"),
            Source("test:a", "test", "stalled"),
            Source("zoom:mic-only", "zoom", "stalled", hasVideo: false),
            Source("zoom:1", "zoom", "stalled"),
        };
        Assert.Equal(1, NetworkCongestionReadout.StalledFeedCount(sources));
        Assert.Null(NetworkCongestionReadout.Format(Snapshot(Level1Queue445, sources)));
    }

    [Fact]
    public void TheSourcesNodeBindsFromTheCoreWireShape()
    {
        // Exactly what core::sourceHealthState emits (SourceVideoIngress.cpp), through the typed
        // observation view that used to omit `sources`.
        const string json = """
            {"sceneId":"s","sources":[
              {"sourceId":"zoom:12","kind":"zoom","width":1920,"height":1080,"framesIngested":1200,"droppedFrames":0,
               "hasVideo":true,"hasAudio":true,"audioPacketsIngested":3000,"audioSamplesIngested":2880000,
               "health":"stalled","dropoutPolicy":"hold","displayName":"Dana"}],
             "outputSenderSession":{"status":"live","activeSenderCount":1,"senders":[
              {"senderId":"rtmp","destination":"rtmp","status":"live",
               "backpressure":{"divisor":2,"appliedDivisor":1,"level":1,"bufferedMs":445,"queuedChunks":3}}]}}
            """;
        var snapshot = JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>(
            CoreObservationModel.TypedJson(JsonDocument.Parse(json).RootElement),
            new JsonSerializerOptions(JsonSerializerDefaults.Web))!;

        var source = Assert.Single(snapshot.Sources);
        Assert.Equal("zoom:12", source.SourceId);
        Assert.Equal("zoom", source.Kind);
        Assert.Equal("stalled", source.Health);
        Assert.True(source.HasVideo);
        Assert.Equal("Dana", source.DisplayName);
        var pressure = Assert.Single(snapshot.OutputSenderSession.Senders).Backpressure!;
        Assert.Equal(2, pressure.Divisor);
        Assert.Equal(1, pressure.AppliedDivisor);
        Assert.Equal(445, pressure.BufferedMs);
    }

    [Fact]
    public void TheDegradedReadoutAndTheCongestionLineAgreeOnWhatPressureIs()
    {
        var snapshot = Snapshot(Level1Queue445, Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled"));
        Assert.NotNull(StreamDegradationReadout.Format(snapshot));
        Assert.NotNull(NetworkCongestionReadout.Format(snapshot));

        var recovered = Snapshot(new() { Divisor = 1, AppliedDivisor = 1, Level = 0 },
            Source("zoom:1", "zoom", "stalled"), Source("zoom:2", "zoom", "stalled"));
        Assert.Null(StreamDegradationReadout.Format(recovered));
        Assert.Null(NetworkCongestionReadout.Format(recovered));
    }
}
