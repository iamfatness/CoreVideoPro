using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>#1074: the stalled-feeds status line. The live show of 2026-10-31 read as participants
/// dropping; the snapshot showed the RTMP queue in backpressure and three or four Zoom feeds stalled
/// at the same second. These pin when the line appears and what it says (two facts, no cause).</summary>
public sealed class StalledFeedsReadoutTests
{
    private static NativeMediaCoreSourceHealth Source(string id, string kind, string health,
        double? sinceLastNewFrameMs = 2000, bool hasVideo = true) =>
        new() { SourceId = id, Kind = kind, Health = health, HasVideo = hasVideo, SinceLastNewFrameMs = sinceLastNewFrameMs };

    private static NativeMediaCoreSourceHealth StalledZoom(string pid, double ageMs = 2000) => Source(pid, "zoom", "stalled", ageMs);

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
    public void TheLiveShowShapeProducesTheLineWithoutClaimingACause()
    {
        // 09:59:33 EDT: `[stream-backpressure] enter divisor=2 buffered=445ms`, three Zoom
        // participants stalled on the same one-second sample. appliedDivisor stays 1 since #538.
        var snapshot = Snapshot(Level1Queue445,
            StalledZoom("1"), StalledZoom("2"), StalledZoom("3"), Source("4", "zoom", "producing", 16));

        var line = NetworkFree(StalledFeedsReadout.Format(snapshot));
        Assert.Equal("Stream 445 ms behind and 3 Zoom feeds stalled", line);
    }

    [Fact]
    public void AnAppliedDivisorIsReportedAsARate()
    {
        var pressure = new NativeMediaCoreSenderBackpressure { Divisor = 3, AppliedDivisor = 3, Level = 2, BufferedMs = 900 };
        Assert.Equal("Stream at 1/3 rate and 2 Zoom feeds stalled",
            NetworkFree(StalledFeedsReadout.Format(Snapshot(pressure, StalledZoom("1"), StalledZoom("2")))));
    }

    [Fact]
    public void ADrainedQueueStillUnderPressureReportsTheLevelNotAnInventedDepth()
    {
        var pressure = new NativeMediaCoreSenderBackpressure { Divisor = 2, AppliedDivisor = 1, Level = 1, BufferedMs = 0 };
        Assert.Equal("Stream backpressure level 1 and 2 Zoom feeds stalled",
            StalledFeedsReadout.Format(Snapshot(pressure, StalledZoom("1"), StalledZoom("2"))));
    }

    // Review finding: two static screen captures read "stalled" on the bus (they re-serve one
    // frame while healthy) and any RTMP pressure used to light "network congested" with no fault.
    [Fact]
    public void StaticScreenCapturesUnderStreamPressureNeverLightTheLine()
    {
        var snapshot = Snapshot(Level1Queue445,
            Source("capture:screen-1", "capture", "stalled", 120_000),
            Source("capture:screen-2", "capture", "stalled", 300_000));
        Assert.Equal(0, StalledFeedsReadout.StalledFeedCount(snapshot.Sources));
        Assert.Null(StalledFeedsReadout.Format(snapshot));
    }

    // Review finding: the bus says "stalled" at 200 ms. Counting that flickers against the
    // 1.5 s on-air rule on ordinary cadence noise. Both edges pinned.
    [Theory]
    [InlineData(200, false)]
    [InlineData(1499, false)]
    [InlineData(1500, true)]
    [InlineData(3000, true)]
    public void AZoomFeedCountsOnlyFromTheOnAirStallThreshold(double ageMs, bool counted)
    {
        var sources = new[] { StalledZoom("1", ageMs), StalledZoom("2", ageMs) };
        Assert.Equal(counted ? 2 : 0, StalledFeedsReadout.StalledFeedCount(sources));
        Assert.Equal(counted, StalledFeedsReadout.Format(Snapshot(Level1Queue445, sources)) is not null);
    }

    [Fact]
    public void ARowWithoutTheAgeFieldIsNeverCounted()
    {
        // An older core publishes `health` but not `sinceLastNewFrameMs`; the 200 ms health alone
        // is not evidence of an on-air stall.
        var sources = new[] { Source("1", "zoom", "stalled", null), Source("2", "zoom", "stalled", null) };
        Assert.Equal(0, StalledFeedsReadout.StalledFeedCount(sources));
    }

    [Fact]
    public void OneStalledFeedIsThatGuestsProblem()
    {
        Assert.Null(StalledFeedsReadout.Format(Snapshot(Level1Queue445, StalledZoom("1"), Source("2", "zoom", "producing", 16))));
    }

    [Fact]
    public void StalledFeedsWithoutStreamBackpressureShowNothing()
    {
        // 09:54:51, before the stream started: all four stalled, no pressured sender. The Tiles
        // hold covers the picture; this line stays quiet because the stream fact is absent.
        var healthy = new NativeMediaCoreSenderBackpressure { Divisor = 1, AppliedDivisor = 1, Level = 0 };
        Assert.Null(StalledFeedsReadout.Format(Snapshot(healthy, StalledZoom("1"), StalledZoom("2"))));
        Assert.Null(StalledFeedsReadout.Format(Snapshot(null, StalledZoom("1"), StalledZoom("2"))));
    }

    [Theory]
    [InlineData("starting")]
    [InlineData("failed")]
    [InlineData("idle")]
    public void ANonProducingSenderDoesNotCountAsBackpressure(string status)
    {
        var snapshot = Snapshot(Level1Queue445, StalledZoom("1"), StalledZoom("2")) with
        {
            OutputSenderSession = new()
            {
                Status = status,
                Senders = [new() { SenderId = "rtmp", Destination = "rtmp", Status = status, Backpressure = Level1Queue445 }]
            }
        };
        Assert.Null(StalledFeedsReadout.Format(snapshot));
    }

    [Fact]
    public void StillsClipsComposedAndAudioOnlySourcesAreNotFeeds()
    {
        var sources = new[]
        {
            Source("still:logo", "still", "stalled", 60_000),
            Source("media:clip", "media", "stalled", 60_000),
            Source("tiles:s", "composed", "stalled", 60_000),
            Source("zoom-slate", "zoom-slate", "stalled", 60_000),
            Source("9", "zoom", "stalled", 60_000, hasVideo: false),
            StalledZoom("1"),
        };
        Assert.Equal(1, StalledFeedsReadout.StalledFeedCount(sources));
        Assert.Null(StalledFeedsReadout.Format(Snapshot(Level1Queue445, sources)));
    }

    [Fact]
    public void TheSourcesNodeBindsFromTheCoreWireShape()
    {
        // Exactly what core::sourceHealthState emits (SourceVideoIngress.cpp): a Zoom row keyed by
        // the bare participant id, with sinceLastNewFrameMs, through the typed observation view.
        const string json = """
            {"sceneId":"s","sources":[
              {"sourceId":"12","kind":"zoom","width":1920,"height":1080,"framesIngested":1200,"droppedFrames":0,
               "hasVideo":true,"hasAudio":true,"audioPacketsIngested":3000,"audioSamplesIngested":2880000,
               "health":"stalled","sinceLastNewFrameMs":1730,"dropoutPolicy":"hold","displayName":"Dana"},
              {"sourceId":"13","kind":"zoom","hasVideo":false,"health":"warming","sinceLastNewFrameMs":null}],
             "outputSenderSession":{"status":"live","activeSenderCount":1,"senders":[
              {"senderId":"rtmp","destination":"rtmp","status":"live",
               "backpressure":{"divisor":2,"appliedDivisor":1,"level":1,"bufferedMs":445,"queuedChunks":3}}]}}
            """;
        var snapshot = JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>(
            CoreObservationModel.TypedJson(JsonDocument.Parse(json).RootElement),
            new JsonSerializerOptions(JsonSerializerDefaults.Web))!;

        Assert.Equal(2, snapshot.Sources.Count);
        var source = snapshot.Sources[0];
        Assert.Equal("12", source.SourceId);
        Assert.Equal("zoom", source.Kind);
        Assert.Equal("stalled", source.Health);
        Assert.Equal(1730, source.SinceLastNewFrameMs);
        Assert.True(source.HasVideo);
        Assert.Equal("Dana", source.DisplayName);
        Assert.Null(snapshot.Sources[1].SinceLastNewFrameMs);
        var pressure = Assert.Single(snapshot.OutputSenderSession.Senders).Backpressure!;
        Assert.Equal(2, pressure.Divisor);
        Assert.Equal(1, pressure.AppliedDivisor);
        Assert.Equal(445, pressure.BufferedMs);
        Assert.Equal(1, StalledFeedsReadout.StalledFeedCount(snapshot.Sources));
    }

    [Fact]
    public void TheDegradedReadoutAndTheStalledFeedsLineAgreeOnWhatPressureIs()
    {
        var snapshot = Snapshot(Level1Queue445, StalledZoom("1"), StalledZoom("2"));
        Assert.NotNull(StreamDegradationReadout.Format(snapshot));
        Assert.NotNull(StalledFeedsReadout.Format(snapshot));

        var recovered = Snapshot(new() { Divisor = 1, AppliedDivisor = 1, Level = 0 }, StalledZoom("1"), StalledZoom("2"));
        Assert.Null(StreamDegradationReadout.Format(recovered));
        Assert.Null(StalledFeedsReadout.Format(recovered));
    }

    /// <summary>The line states symptoms; it must never assert a network cause it cannot see.</summary>
    private static string? NetworkFree(string? line)
    {
        Assert.NotNull(line);
        Assert.DoesNotContain("network", line, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("congest", line, StringComparison.OrdinalIgnoreCase);
        return line;
    }
}
