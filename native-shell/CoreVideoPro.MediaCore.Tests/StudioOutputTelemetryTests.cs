using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class StudioOutputTelemetryTests
{
    [Theory]
    [InlineData("preparing", "healthy", false, "Starting", "idle")]
    [InlineData("producing", "healthy", false, "Recording", "live")]
    [InlineData("producing", "degraded", false, "Degraded", "warning")]
    [InlineData("interrupted", "failed", false, "Interrupted", "warning")]
    [InlineData("failed", "failed", false, "Failed", "warning")]
    [InlineData("stopping", "healthy", false, "Stopping", "idle")]
    [InlineData("finalizing", "healthy", false, "Finalizing", "idle")]
    [InlineData("completed", "healthy", true, "Completed", "idle")]
    [InlineData("completed", "healthy", false, "Failed", "warning")]
    public void RecordingUsesObservedLifecycleEvenWhenActiveFlagDisagrees(string state, string health, bool finalized, string expected, string expectedHealth)
    {
        var actual = StudioOutputTelemetry.Recording(Recording(Lifecycle(state, health, finalized), 120));
        Assert.Equal(expected, actual.State);
        Assert.Equal(expectedHealth, actual.Health);
        Assert.Equal($"{120:N0} frames", actual.Progress);
    }

    [Fact]
    public void MissingLifecycleCannotTurnAnActiveFlagOrSenderRequestIntoVerifiedOutput()
    {
        Assert.Equal("Unverified", StudioOutputTelemetry.Recording(Recording(null, 0)).State);
        Assert.Equal("Unverified", StudioOutputTelemetry.Streaming([Sender(null, 0)]).State);
        Assert.Equal("Idle", StudioOutputTelemetry.Recording(null).State);
        Assert.Equal("Idle", StudioOutputTelemetry.Streaming([]).State);
    }

    [Fact]
    public void RecordingFrameCountFollowsWriterProgressAndResetsForNewSession()
    {
        Assert.NotEqual(StudioOutputTelemetry.Recording(Recording(Lifecycle("producing"), 120)).Progress,
            StudioOutputTelemetry.Recording(Recording(Lifecycle("producing"), 180)).Progress);
        Assert.Equal("0 frames", StudioOutputTelemetry.Recording(Recording(Lifecycle("preparing"), 0)).Progress);
    }

    [Fact]
    public void StreamProgressComesFromSendersAndPartialFailureNeverLooksHealthy()
    {
        var active = StudioOutputTelemetry.Streaming([Sender(Lifecycle("producing"), 120)]);
        Assert.Equal("Streaming", active.State);
        Assert.Equal("live", active.Health);
        Assert.Equal($"{120:N0} frames", active.Progress);
        var partial = StudioOutputTelemetry.Streaming([Sender(Lifecycle("producing"), 120), Sender(Lifecycle("failed", "failed"), 20)]);
        Assert.Equal("Partial", partial.State);
        Assert.Equal("warning", partial.Health);
        Assert.Equal($"{140:N0} frames", partial.Progress);
    }

    [Fact]
    public void EmptySessionIdentityCannotClaimProducing()
    {
        var lifecycle = Lifecycle("producing") with { SessionId = "" };
        Assert.Equal("Unverified", StudioOutputTelemetry.Recording(Recording(lifecycle, 120)).State);
    }

    [Fact]
    public void FailureBeforeSenderCreationDoesNotDisappearIntoIdle()
    {
        var snapshot = new NativeMediaCoreStateSnapshot { OutputHealth = [new() { Destination = "rtmp", Status = "failed", Message = "Destination refused" }] };
        Assert.Equal("Failed", StudioOutputTelemetry.Streaming(snapshot).State);
        Assert.Equal("warning", StudioOutputTelemetry.Streaming(snapshot).Health);
    }

    [Fact]
    public void LegacySenderFailureRemainsVisibleWithoutInventingVerifiedDelivery()
    {
        var sender = new NativeMediaCoreOutputSender { SenderId = "failed", Destination = "rtmp", Status = "failed" };
        Assert.Equal("Failed", StudioOutputTelemetry.Streaming([sender]).State);
        Assert.Equal("warning", StudioOutputTelemetry.Streaming([sender]).Health);
    }

    [Fact]
    public void ProgramRenderingDoesNotInheritAnUnrelatedStreamFailure()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            ProgramFrameCount = 120, Compositor = new() { Status = "live" },
            OutputHealth = [new() { Destination = "rtmp", Status = "failed", Message = "Destination refused" }]
        };
        Assert.Equal("Rendering", StudioOutputTelemetry.Program(snapshot).State);
        Assert.Equal("Failed", StudioOutputTelemetry.Streaming(snapshot).State);
    }

    [Fact]
    public void ProgramMustPublishAFrameBeforeReportingRendering()
    {
        var snapshot = new NativeMediaCoreStateSnapshot { Compositor = new() { Status = "live" } };
        Assert.Equal("Starting", StudioOutputTelemetry.Program(snapshot).State);
    }

    [Fact]
    public void AnIntentionallyEndedDestinationDoesNotTurnTheCurrentStreamIntoPartialFailure()
    {
        var ended = Sender(Lifecycle("completed", finalized: true) with { DesiredActive = false }, 400);
        var active = Sender(Lifecycle("producing"), 120);
        var snapshot = new NativeMediaCoreStateSnapshot { OutputSenderSession = new() { Status = "live", Senders = [ended, active] } };
        var readout = StudioOutputTelemetry.Streaming(snapshot);
        Assert.Equal("Streaming", readout.State);
        Assert.Equal("live", readout.Health);
        Assert.Equal($"{120:N0} frames", readout.Progress);
    }

    internal static OutputLifecycle Lifecycle(string state, string health = "healthy", bool finalized = false) => new()
    { SessionId = "test-session", DesiredActive = true, State = state, Health = health, Finalized = finalized };

    private static NativeMediaCoreRecordingSession Recording(OutputLifecycle? lifecycle, int frames) => new()
    {
        SessionId = "test-session", Active = true, Status = "recording", WriterStatus = "writing", Lifecycle = lifecycle,
        TargetFolder = "isolated", FilenamePrefix = "test", Format = "mp4", Quality = "high", ProgramPath = "isolated/Program.mp4", TotalFramesWritten = frames
    };

    [Fact]
    public void StreamingIgnoresNdiSenders()
    {
        var rtmp = Sender(null, 100);
        var ndi = Sender(null, 900, "ndi");

        var both = StudioOutputTelemetry.Streaming([rtmp, ndi]);
        Assert.Equal($"{100:N0} frames", both.Progress);
        Assert.NotEqual("Partial", both.State);

        Assert.Equal("Idle", StudioOutputTelemetry.Streaming([ndi]).State);
        Assert.Equal("Idle", StudioOutputTelemetry.Streaming([Sender(null, 900, "ndi-graphics")]).State);
    }

    private static NativeMediaCoreOutputSender Sender(OutputLifecycle? lifecycle, int frames, string destination = "rtmp") => new()
    { SenderId = "test-sender", Destination = destination, Status = "live", Lifecycle = lifecycle, FramesSent = frames, BitrateMbps = 99 };
}
