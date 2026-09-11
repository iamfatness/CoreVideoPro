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
    internal static OutputLifecycle Lifecycle(string state, string health = "healthy", string session = "session-1") => new()
    {
        SessionId = session,
        DesiredActive = state is not ("stopping" or "finalizing" or "completed" or "idle"),
        State = state,
        Health = health,
        Finalized = state == "completed"
    };

    internal static NativeMediaCoreRecordingSession Recording(
        string? lifecycleState, bool active = false, string status = "idle", string session = "session-1") => new()
    {
        Lifecycle = lifecycleState is null ? null : Lifecycle(lifecycleState, session: session),
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

    internal static CloseGuardInput Input(
        NativeMediaCoreStateSnapshot? snapshot = null,
        bool coreRunning = true,
        bool recordingRequested = false,
        bool recording = false,
        bool streamingRequested = false,
        bool streaming = false,
        bool recordingInFlight = false,
        bool streamInFlight = false,
        long generation = 0) =>
        new(coreRunning, recordingRequested, recording, streamingRequested, streaming, snapshot,
            recordingInFlight, streamInFlight, generation);

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
        // Fix round 1 (finding 10): an older core with no lifecycle that says it IS recording is
        // core evidence too; either source saying live is enough.
        Assert.True(CloseGuardPolicy.Evaluate(Input(Snapshot(Recording(null, active: true, status: "recording")))).ShouldAsk);
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

    // --- "finished" is tied to THIS stop (fix round 1, finding 1) ---

    private static OutputStopBaseline BaselineOf(CloseGuardInput atClose) => CloseGuardPolicy.Evaluate(atClose).Baseline;

    private static readonly CloseGuardInput Quiet = Input();

    [Fact]
    public void SettledNeedsEvidence()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing")), recordingRequested: true));

        Assert.Equal(OutputSettleState.Pending, CloseGuardPolicy.EvaluateSettled(null, baseline, Quiet).State);
    }

    [Fact]
    public void BaselineNamesTheLiveSessionAndSenders()
    {
        var baseline = BaselineOf(Input(
            Snapshot(Recording("producing", session: "rec-7"), Sender("rtmp://a", "producing"), Sender("rtmp://old", "completed")),
            recordingRequested: true, streamingRequested: true, generation: 3));

        Assert.Equal("rec-7", baseline.RecordingSessionId);
        Assert.Equal(["rtmp://a"], baseline.Senders.Keys);
        Assert.Equal(3, baseline.CoreGeneration);
    }

    [Fact]
    public void SettledWhenTheBaselineSessionCompletedAndSendersStopped()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing"), Sender("rtmp://a", "producing")), recordingRequested: true, streamingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed"), Sender("rtmp://a", "completed")), baseline, Quiet);

        Assert.Equal(OutputSettleState.Settled, result.State);
    }

    [Fact]
    public void FinalizingIsPendingButTheStopHasLanded()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing")), recordingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("finalizing")), baseline, Quiet);

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.False(result.StopNotYetLanded);
    }

    [Fact]
    public void ProducingIsPendingAndTheStopHasNotLanded()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing"), Sender("rtmp://a", "producing")), recordingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed"), Sender("rtmp://a", "producing")), baseline, Quiet);

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.True(result.StopNotYetLanded);
    }

    [Theory]
    [InlineData("failed")]
    [InlineData("interrupted")]
    public void AFailedBaselineSessionSettlesWithFailure(string state)
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing")), recordingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording(state)), baseline, Quiet);

        Assert.Equal(OutputSettleState.SettledWithFailure, result.State);
    }

    [Fact]
    public void IntentStillSetIsPendingEvenOverATerminalSnapshot()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing")), recordingRequested: true));

        var requested = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed")), baseline, Input(recordingRequested: true));
        var inFlight = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed")), baseline, Input(recordingInFlight: true));

        Assert.Equal(OutputSettleState.Pending, requested.State);
        Assert.True(requested.StopNotYetLanded);
        Assert.Equal(OutputSettleState.Pending, inFlight.State);
    }

    [Fact]
    public void StartInFlightWithAPreviousCompletedStaysPendingUntilTheNewSessionCompletes()
    {
        // The operator pressed Record, the start is still in flight, and the snapshot still shows
        // the PREVIOUS session completed. That completion is history, not this stop's evidence.
        var atClose = Input(Snapshot(Recording("completed", session: "old")), recordingRequested: true, recordingInFlight: true);
        var baseline = BaselineOf(atClose);
        Assert.Null(baseline.RecordingSessionId);
        Assert.Equal("old", baseline.StaleRecordingSessionId);

        // Start still in flight: pending, whatever the snapshot says.
        Assert.Equal(OutputSettleState.Pending,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed", session: "old")), baseline, Input(recordingRequested: true, recordingInFlight: true)).State);
        // The start landed: the new session runs, so the stop has not landed.
        var running = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("producing", session: "new")), baseline, Quiet);
        Assert.Equal(OutputSettleState.Pending, running.State);
        Assert.True(running.StopNotYetLanded);
        // Finalizing, then completed.
        Assert.Equal(OutputSettleState.Pending,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("finalizing", session: "new")), baseline, Quiet).State);
        Assert.Equal(OutputSettleState.Settled,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed", session: "new")), baseline, Quiet).State);
    }

    [Fact]
    public void AnAbsentRecordingNodeIsPendingNeverFinished()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing")), recordingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(recording: null), baseline, Quiet);

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.Contains("no recording evidence", result.Detail);
    }

    [Fact]
    public void AnAbsentBaselineSenderIsPendingNeverFinished()
    {
        var baseline = BaselineOf(Input(Snapshot(null, Sender("rtmp://a", "producing")), streamingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(), baseline, Quiet);

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.Contains("no evidence for rtmp://a", result.Detail);
    }

    [Fact]
    public void ARecordingFromAnotherSessionIsNotThisStopsEvidence()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing", session: "rec-1")), recordingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("idle", session: "rec-2")), baseline, Quiet);

        Assert.Equal(OutputSettleState.Pending, result.State);
    }

    [Fact]
    public void AnOldFailureDoesNotYieldFinishedWithFailureForAStreamOnlyStop()
    {
        // The recording failed an hour ago; only streaming was live at the close.
        var baseline = BaselineOf(Input(Snapshot(Recording("failed", session: "hour-ago"), Sender("rtmp://a", "producing")), streamingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("failed", session: "hour-ago"), Sender("rtmp://a", "completed")), baseline, Quiet);

        Assert.Equal(OutputSettleState.Settled, result.State);
    }

    [Fact]
    public void AStreamStartedDuringTheWaitKeepsItPending()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording("producing")), recordingRequested: true));

        var result = CloseGuardPolicy.EvaluateSettled(Snapshot(Recording("completed"), Sender("srt://new", "producing")), baseline, Quiet);

        Assert.Equal(OutputSettleState.Pending, result.State);
        Assert.True(result.StopNotYetLanded);
    }

    [Fact]
    public void LegacyRecordingWithoutLifecycleSettlesWhenInactive()
    {
        var baseline = BaselineOf(Input(Snapshot(Recording(null, active: true, status: "recording"))));
        Assert.True(baseline.LegacyRecording);

        Assert.Equal(OutputSettleState.Pending,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording(null, active: true, status: "recording")), baseline, Quiet).State);
        Assert.Equal(OutputSettleState.Pending,
            CloseGuardPolicy.EvaluateSettled(Snapshot(recording: null), baseline, Quiet).State);
        Assert.Equal(OutputSettleState.Settled,
            CloseGuardPolicy.EvaluateSettled(Snapshot(Recording(null, active: false, status: "stopped")), baseline, Quiet).State);
    }

    [Fact]
    public void MergeKeepsTheCloseRequestGenerationAndWidensSenders()
    {
        var atClose = BaselineOf(Input(Snapshot(Recording("producing", session: "rec-1")), recordingRequested: true, generation: 2));
        var atStop = BaselineOf(Input(Snapshot(Recording("producing", session: "rec-1"), Sender("rtmp://late", "producing")),
            recordingRequested: true, streamingRequested: true, generation: 2));

        var merged = CloseGuardPolicy.Merge(atClose, atStop);

        Assert.Equal(2, merged.CoreGeneration);
        Assert.Equal("rec-1", merged.RecordingSessionId);
        Assert.True(merged.StreamingExpected);
        Assert.Contains("rtmp://late", merged.Senders.Keys);
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
        // The clean-exit path's stderr drain never costs more than the kill path it replaces.
        Assert.True(ShutdownBudget.CoreExitGrace + TimeSpan.FromMilliseconds(MediaCoreSupervisor.StderrDrainMilliseconds)
                    <= ShutdownBudget.CoreStopWorstCase);
    }

    [Fact]
    public void OutputsFinishBoundIsFifteenSeconds()
    {
        Assert.Equal(TimeSpan.FromSeconds(15), OutputShutdownCoordinator.FinishTimeout);
    }
}
