using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.8 (#461): closing while recording or streaming asks first. These pin WHEN it asks, what the
/// dialog says, and when the outputs count as finished.
/// </summary>
public sealed class CloseGuardPolicyTests
{
    internal static OutputLifecycle Lifecycle(string state, string health = "healthy") => new()
    {
        SessionId = "session-1",
        DesiredActive = state is not ("stopping" or "finalizing" or "completed" or "idle"),
        State = state,
        Health = health,
        Finalized = state == "completed"
    };

    internal static NativeMediaCoreRecordingSession Recording(string? lifecycleState, bool active = false, string status = "idle") => new()
    {
        Lifecycle = lifecycleState is null ? null : Lifecycle(lifecycleState),
        SessionId = "session-1",
        Active = active,
        Status = status,
        WriterStatus = "idle",
        TargetFolder = "C:\\rec",
        FilenamePrefix = "show",
        Format = "mp4",
        Quality = "high",
        ProgramPath = "C:\\rec\\show.mp4"
    };

    internal static NativeMediaCoreOutputSender Sender(string destination, string? lifecycleState, string status = "live") => new()
    {
        SenderId = destination,
        Destination = destination,
        Status = status,
        Lifecycle = lifecycleState is null ? null : Lifecycle(lifecycleState)
    };

    internal static NativeMediaCoreStateSnapshot Snapshot(
        NativeMediaCoreRecordingSession? recording = null,
        params NativeMediaCoreOutputSender[] senders) => new()
    {
        Recording = recording,
        OutputSenderSession = new NativeMediaCoreOutputSenderSession
        {
            Status = senders.Length > 0 ? "live" : "idle",
            ActiveSenderCount = senders.Length,
            Senders = senders
        }
    };

    private static CloseGuardInput Input(
        NativeMediaCoreStateSnapshot? snapshot = null,
        bool coreRunning = true,
        bool recordingRequested = false,
        bool recording = false,
        bool streamingRequested = false,
        bool streaming = false) =>
        new(coreRunning, recordingRequested, recording, streamingRequested, streaming, snapshot);

    [Fact]
    public void NothingLiveDoesNotAsk()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(Snapshot(Recording("completed"), Sender("rtmp", "completed"))));

        Assert.False(decision.ShouldAsk);
    }

    [Fact]
    public void ProducingRecordingAsks()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(Snapshot(Recording("producing")), recordingRequested: true, recording: true));

        Assert.True(decision.ShouldAsk);
        Assert.Equal("Recording", decision.Description);
        Assert.Equal(
            "Recording. Closing now would cut the recording off before it's saved.",
            CloseGuardPolicy.DialogBody(decision));
    }

    [Theory]
    [InlineData("stopping")]
    [InlineData("finalizing")]
    public void RecordingThatIsStillFinalizingStillAsks(string state)
    {
        // The operator already pressed Stop (flags are off), but the writer has not written the
        // file's index yet: closing now still corrupts the file.
        var decision = CloseGuardPolicy.Evaluate(Input(Snapshot(Recording(state))));

        Assert.True(decision.ShouldAsk);
        Assert.Equal("Recording is still finalizing", decision.Description);
    }

    [Theory]
    [InlineData("requested")]
    [InlineData("preparing")]
    [InlineData("starting")]
    [InlineData("live")]
    public void EveryPreTerminalRecordingStateAsks(string state)
    {
        Assert.True(CloseGuardPolicy.Evaluate(Input(Snapshot(Recording(state)))).ShouldAsk);
    }

    [Theory]
    [InlineData("completed")]
    [InlineData("failed")]
    [InlineData("interrupted")]
    [InlineData("idle")]
    public void TerminalOrIdleRecordingWithNoFlagsDoesNotAsk(string state)
    {
        Assert.False(CloseGuardPolicy.Evaluate(Input(Snapshot(Recording(state)))).ShouldAsk);
    }

    [Fact]
    public void AbsentLifecycleTrustsTheShellFlag()
    {
        Assert.True(CloseGuardPolicy.Evaluate(Input(Snapshot(Recording(null)), recordingRequested: true)).ShouldAsk);
        Assert.True(CloseGuardPolicy.Evaluate(Input(snapshot: null, recording: true)).ShouldAsk);
        Assert.False(CloseGuardPolicy.Evaluate(Input(Snapshot(Recording(null, active: true, status: "recording")))).ShouldAsk);
    }

    [Fact]
    public void StreamingCountsLiveDestinations()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(
            Snapshot(null, Sender("rtmp://a", "producing"), Sender("srt://b", "producing"), Sender("rtmp://old", "completed")),
            streamingRequested: true,
            streaming: true));

        Assert.True(decision.ShouldAsk);
        Assert.Equal(2, decision.LiveStreamDestinations);
        Assert.Equal("Streaming to 2 destinations", decision.Description);
    }

    [Fact]
    public void OneDestinationIsSingular()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(Snapshot(null, Sender("rtmp://a", "producing")), streamingRequested: true));

        Assert.Equal("Streaming to 1 destination", decision.Description);
    }

    [Fact]
    public void StreamingFlagWithoutSenderLifecycleSaysStreaming()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(Snapshot(null, Sender("rtmp://a", lifecycleState: null)), streaming: true));

        Assert.True(decision.ShouldAsk);
        Assert.Equal("Streaming", decision.Description);
    }

    [Fact]
    public void StoppingSenderIsStillLive()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(Snapshot(null, Sender("rtmp://a", "stopping"))));

        Assert.True(decision.ShouldAsk);
        Assert.Equal("A stream is still stopping", decision.Description);
    }

    [Fact]
    public void RecordingAndStreamingAreBothNamed()
    {
        var decision = CloseGuardPolicy.Evaluate(Input(
            Snapshot(Recording("producing"), Sender("rtmp://a", "producing"), Sender("ndi", "producing")),
            recordingRequested: true,
            streamingRequested: true));

        Assert.True(decision.RecordingActive);
        Assert.True(decision.StreamingActive);
        Assert.Equal("Recording and streaming to 2 destinations", decision.Description);
    }

    [Fact]
    public void VirtualCameraAloneDoesNotAsk()
    {
        var snapshot = Snapshot() with
        {
            VirtualCamera = new NativeMediaCoreVirtualCamera { Enabled = true, Status = "live", FramesPublished = 5000 }
        };

        Assert.False(CloseGuardPolicy.Evaluate(Input(snapshot)).ShouldAsk);
    }

    [Fact]
    public void NoCoreMeansNothingLeftToSave()
    {
        Assert.False(CloseGuardPolicy.Evaluate(Input(Snapshot(Recording("producing")), coreRunning: false, recordingRequested: true)).ShouldAsk);
    }

    [Fact]
    public void DialogTextIsExact()
    {
        Assert.Equal("Stop outputs and close?", CloseGuardPolicy.DialogTitle);
        Assert.Equal("Stop and close", CloseGuardPolicy.StopAndCloseButton);
        Assert.Equal("Keep running", CloseGuardPolicy.KeepRunningButton);
        Assert.Equal("Closing now would cut the recording off before it's saved.", CloseGuardPolicy.DialogConsequence);
    }

    [Fact]
    public void SettledNeedsEvidence()
    {
        Assert.Equal(OutputSettleState.Pending, CloseGuardPolicy.EvaluateSettled(null).State);
    }

    [Fact]
    public void SettledWhenRecordingCompletedAndSendersStopped()
    {
        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed"), Sender("rtmp://a", "completed")));

        Assert.Equal(OutputSettleState.Settled, result.State);
    }

    [Fact]
    public void FinalizingIsPendingButTheStopHasLanded()
    {
        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("finalizing")));

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.False(result.StopNotYetLanded);
    }

    [Fact]
    public void ProducingIsPendingAndTheStopHasNotLanded()
    {
        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed"), Sender("rtmp://a", "producing")));

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.True(result.StopNotYetLanded);
    }

    [Theory]
    [InlineData("failed")]
    [InlineData("interrupted")]
    public void FailedDestinationSettlesWithFailure(string state)
    {
        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording(state)));

        Assert.Equal(OutputSettleState.SettledWithFailure, result.State);
    }

    [Fact]
    public void LegacyRecordingWithoutLifecycleSettlesWhenInactive()
    {
        Assert.Equal(OutputSettleState.Pending,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording(null, active: true, status: "recording"))).State);
        Assert.Equal(OutputSettleState.Settled,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording(null, active: false, status: "stopped"))).State);
    }
}

/// <summary>The core's app-exit grace must fit inside the close budget (T1.8).</summary>
public sealed class ShutdownBudgetTests
{
    [Fact]
    public void CoreStopWorstCaseLeavesRoomInsideTheShutdownTimeout()
    {
        Assert.Equal(TimeSpan.FromSeconds(2), ShutdownBudget.CoreExitGrace);
        Assert.Equal(1500, MediaCoreSupervisor.KillWaitMilliseconds);
        // Grace that runs out + the kill-tree's own wait, with at least a second left for the
        // rest of the view-model disposal before the 5 s timeout.
        Assert.True(ShutdownBudget.CoreStopWorstCase + TimeSpan.FromSeconds(1) <= ShutdownBudget.ShutdownTimeout);
        Assert.True(ShutdownBudget.ShutdownTimeout < ShutdownBudget.ShutdownWatchdog);
    }

    [Fact]
    public void OutputsFinishBoundIsFifteenSeconds()
    {
        Assert.Equal(TimeSpan.FromSeconds(15), OutputShutdownCoordinator.FinishTimeout);
    }
}
