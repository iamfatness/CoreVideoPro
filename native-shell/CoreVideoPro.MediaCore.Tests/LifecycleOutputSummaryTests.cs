using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class LifecycleOutputSummaryTests
{
    private static NativeMediaCoreStateSnapshot Snapshot(string recordingState, params NativeMediaCoreOutputSender[] senders) => new()
    {
        Recording = new NativeMediaCoreRecordingSession
        {
            SessionId = "session-1", Status = "idle", WriterStatus = "idle", TargetFolder = "", FilenamePrefix = "",
            Format = "mp4", Quality = "high", ProgramPath = "",
            Lifecycle = new OutputLifecycle { SessionId = "session-1", DesiredActive = false,
                State = recordingState, Health = "unknown", Finalized = recordingState == "completed" }
        },
        OutputSenderSession = new NativeMediaCoreOutputSenderSession { Status = "live", Senders = senders }
    };

    private static NativeMediaCoreOutputSender Sender(string destination, string status, string? error = null) => new()
    { SenderId = destination + ":program", Destination = destination, Status = status, LastError = error };

    [Theory]
    [InlineData("idle")]
    [InlineData("completed")]
    [InlineData("finalizing")]
    [InlineData("failed")]
    public void RecordingDoesNotHideIndependentLiveStream(string state)
    {
        var snapshot = Snapshot(state, Sender("rtmp", "live"));
        foreach (var summary in new[] { MediaCoreBridgeService.SummarizeOutputs(snapshot), LiveProductionSync.SummarizeOutputSession(snapshot) })
        {
            Assert.Contains("Live: RTMP", summary);
            if (state == "finalizing") Assert.Contains("not ready", summary);
            if (state == "failed") Assert.Contains("Recording failed", summary);
        }
    }

    [Fact]
    public void PartialStreamFailureShowsFailedAndHealthyDestinations()
    {
        var snapshot = Snapshot("idle", Sender("rtmp", "live"), Sender("srt", "failed", "Connection refused"));
        foreach (var summary in new[] { MediaCoreBridgeService.SummarizeOutputs(snapshot), LiveProductionSync.SummarizeOutputSession(snapshot) })
        {
            Assert.Contains("SRT output failed: Connection refused", summary);
            Assert.Contains("Live: RTMP", summary);
        }
    }

    [Fact]
    public void StartingStreamIsVisibleButNotLiveDuringRecordingFinalization()
    {
        var summary = MediaCoreBridgeService.SummarizeOutputs(Snapshot("finalizing", Sender("rtmp", "starting")));
        Assert.Contains("Recording finalizing", summary);
        Assert.Contains("Starting: RTMP", summary);
        Assert.DoesNotContain("Live:", summary);
    }
}
