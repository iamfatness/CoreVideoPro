using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class OutputSessionClockTests
{
    private sealed class Clock : TimeProvider
    {
        public long Ticks;
        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() => Ticks;
        public void Advance(int ms) => Ticks += ms;
    }

    private static NativeMediaCoreStateSnapshot Stream(string state) => new()
    {
        OutputSenderSession = new()
        {
            Status = state,
            Senders = [new()
            {
                SenderId = "test", Destination = "rtmp", Status = state,
                Lifecycle = new() { SessionId = "test", State = state, Health = "healthy", DesiredActive = true, Finalized = false }
            }]
        }
    };

    [Theory]
    [InlineData(1)]
    [InlineData(4)]
    [InlineData(60)]
    public void ElapsedDoesNotDependOnPollOrCaptureCadence(int pollsPerSecond)
    {
        var time = new Clock();
        var clock = new OutputSessionClock(time);
        Assert.Equal(0, clock.Observe(Stream("producing")));
        for (var tick = 1; tick <= pollsPerSecond * 60; tick++)
        {
            time.Ticks = tick * 60000L / (pollsPerSecond * 60);
            clock.Observe(Stream("producing"));
        }
        Assert.Equal(60, clock.Observe(Stream("producing")));
    }

    [Fact]
    public void IdleAndConnectingDoNotCountAsLiveAndNewSessionStartsAtZero()
    {
        var time = new Clock(); var clock = new OutputSessionClock(time);
        clock.Observe(Stream("idle")); time.Advance(120000);
        Assert.Equal(0, clock.Observe(Stream("preparing")));
        time.Advance(30000);
        Assert.Equal(0, clock.Observe(Stream("producing")));
        time.Advance(10000);
        Assert.Equal(10, clock.Observe(Stream("producing")));
        Assert.Equal(0, clock.Observe(Stream("completed")));
        time.Advance(20000);
        Assert.Equal(0, clock.Observe(Stream("producing")));
    }

    [Fact]
    public void InterruptionRetryAndFinalizationRemainInTheSameSession()
    {
        var time = new Clock(); var clock = new OutputSessionClock(time);
        clock.Observe(Stream("producing")); time.Advance(10000);
        Assert.Equal(10, clock.Observe(Stream("interrupted")));
        time.Advance(5000);
        Assert.Equal(15, clock.Observe(Stream("preparing")));
        time.Advance(5000);
        Assert.Equal(20, clock.Observe(Stream("producing")));
        time.Advance(1000);
        Assert.Equal(21, clock.Observe(Stream("finalizing")));
        Assert.Equal(0, clock.Observe(Stream("failed")));
    }

    [Fact]
    public void UnverifiedLegacySenderDoesNotStartClock()
    {
        var time = new Clock(); var clock = new OutputSessionClock(time);
        var unverified = new NativeMediaCoreStateSnapshot
        {
            OutputSenderSession = new()
            {
                Status = "warning", Senders = [new()
                {
                    SenderId = "test", Destination = "rtmp", Status = "warning", FramesSent = 0
                }]
            }
        };
        clock.Observe(unverified); time.Advance(10000);
        Assert.Equal(0, clock.Observe(unverified));
    }

    [Fact]
    public void RecordingKeepsSessionWhenStreamEnds()
    {
        var time = new Clock(); var clock = new OutputSessionClock(time);
        clock.Observe(Stream("producing")); time.Advance(10000);
        var recording = Stream("completed") with
        {
            Recording = new()
            {
                SessionId = "record", Active = true, Status = "recording", WriterStatus = "writing",
                TargetFolder = "", FilenamePrefix = "", Format = "mp4", Quality = "high", ProgramPath = "",
                Lifecycle = new() { SessionId = "record", State = "producing", Health = "healthy", DesiredActive = true, Finalized = false }
            }
        };
        Assert.Equal(10, clock.Observe(recording));
        time.Advance(10000);
        Assert.Equal(20, clock.Observe(recording));
    }
}
