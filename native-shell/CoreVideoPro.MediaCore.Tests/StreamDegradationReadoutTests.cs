using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class StreamDegradationReadoutTests
{
    private static NativeMediaCoreStateSnapshot Snapshot(int fps, string status, int applied, int level = 0) => new()
    {
        OutputProfile = new() { ProfileId = "test", Resolution = "1920x1080", Fps = fps },
        OutputSenderSession = new()
        {
            Status = status,
            Senders = [new() { SenderId = "rtmp", Destination = "rtmp", Status = status,
                Backpressure = new() { AppliedDivisor = applied, Level = level } }]
        }
    };

    [Theory]
    [InlineData(60, 4, "15 fps feed")]
    [InlineData(30, 4, "7.5 fps feed")]
    [InlineData(25, 3, "8.33 fps feed")]
    public void CadenceUsesConfiguredProgramRateAndAppliedDivisor(int fps, int divisor, string expected)
    {
        var snapshot = Snapshot(fps, "live", divisor);
        Assert.Contains(expected, LiveProductionSync.SummarizeStreamStat(snapshot, true, "unused"));
        Assert.Equal("warning", LiveProductionSync.ResolveStreamHealthStatus(snapshot, true));
    }

    [Fact]
    public void RequestedDivisorDoesNotPretendTheActualFeedWasReduced()
    {
        var sender = JsonSerializer.Deserialize<NativeMediaCoreOutputSender>(
            """{"senderId":"rtmp","destination":"rtmp","status":"live","backpressure":{"divisor":4,"appliedDivisor":1,"level":3,"bufferedMs":0}}""",
            new JsonSerializerOptions(JsonSerializerDefaults.Web))!;
        var snapshot = Snapshot(60, "live", 1) with
        {
            OutputSenderSession = new() { Status = "live", Senders = [sender] }
        };
        Assert.Equal("RTMP degraded · 60 fps feed", StreamDegradationReadout.Format(snapshot));
        Assert.Equal("warning", LiveProductionSync.ResolveStreamHealthStatus(snapshot, true));
    }

    [Theory]
    [InlineData("starting")]
    [InlineData("failed")]
    [InlineData("idle")]
    public void NonProducingStatusDoesNotAdvertiseDegradedStreaming(string status) =>
        Assert.Null(StreamDegradationReadout.Format(Snapshot(60, status, 4, 3)));

    [Fact]
    public void RecoveryClearsTheDegradedReadout() =>
        Assert.Null(StreamDegradationReadout.Format(Snapshot(60, "live", 1, 0)));

    [Fact]
    public void FailureOnAnotherDestinationTakesPriorityOverDegradation()
    {
        var snapshot = Snapshot(60, "live", 4) with
        {
            OutputHealth = [new() { Destination = "rtmp", Status = "live", Message = "healthy" },
                new() { Destination = "srt", Status = "failed", Message = "unreachable" }]
        };
        Assert.Equal("failed", LiveProductionSync.ResolveStreamHealthStatus(snapshot, true));
    }

    [Theory]
    [InlineData(0, 4)]
    [InlineData(60, 0)]
    public void MissingCadenceFactsAreUnknown(int fps, int applied) =>
        Assert.Contains("feed rate unknown", StreamDegradationReadout.Format(Snapshot(fps, "live", applied, 3)));
}
