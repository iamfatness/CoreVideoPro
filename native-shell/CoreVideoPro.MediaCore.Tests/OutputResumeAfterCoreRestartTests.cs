using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// #732: a core crash mid-recording must not cost the rest of the show's recording.
public sealed class OutputResumeAfterCoreRestartTests
{
    [Fact]
    public void ARecordingLostToACrashIsResumedOnceZoomIsBack()
    {
        var resume = new OutputResumeAfterCoreRestart(requiresMeeting: true);
        resume.CoreCrashed(outputWasOn: true);
        Assert.True(resume.Owed);

        Assert.Equal(OutputResumeDecision.Resume, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
        // Decided once: a second completion must not start a second session.
        Assert.Equal(OutputResumeDecision.None, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
    }

    [Fact]
    public void NothingIsStartedWhenNothingWasRecording()
    {
        var resume = new OutputResumeAfterCoreRestart(requiresMeeting: true);
        resume.CoreCrashed(outputWasOn: false);
        Assert.Equal(OutputResumeDecision.None, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
    }

    // The shell clears its Recording intent the moment the core starts recovering. A second
    // crash before the resume therefore reports "not recording", and must not erase the first.
    [Fact]
    public void ASecondCrashBeforeTheResumeDoesNotForgetTheRecording()
    {
        var resume = new OutputResumeAfterCoreRestart(requiresMeeting: true);
        resume.CoreCrashed(outputWasOn: true);
        resume.CoreCrashed(outputWasOn: false);
        Assert.Equal(OutputResumeDecision.Resume, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
    }

    [Fact]
    public void WithoutTheMeetingTheRecordingIsNotResumedAndSaysSo()
    {
        foreach (var outcome in new[] { MediaCoreRecoveryOutcome.ZoomRejoinFailed, MediaCoreRecoveryOutcome.CoreOnly })
        {
            var resume = new OutputResumeAfterCoreRestart(requiresMeeting: true);
            resume.CoreCrashed(outputWasOn: true);
            Assert.Equal(OutputResumeDecision.NotResumed, resume.RecoveryCompleted(outcome));
            Assert.Contains("NOT resumed", OutputResumeAfterCoreRestart.DescribeRecordingNotResumed(outcome));
            Assert.False(resume.Owed);
        }
    }

    // The operator stopping (or restarting) Record during the recovery is a decision. Nothing
    // may be started behind it when the core comes back.
    [Fact]
    public void AnOperatorChoiceDuringRecoveryStands()
    {
        var resume = new OutputResumeAfterCoreRestart(requiresMeeting: true);
        resume.CoreCrashed(outputWasOn: true);
        resume.OperatorChose();
        Assert.Equal(OutputResumeDecision.None, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
    }

    [Fact]
    public void ADeliberateCoreStopCancelsTheResume()
    {
        var resume = new OutputResumeAfterCoreRestart(requiresMeeting: true);
        resume.CoreCrashed(outputWasOn: true);
        resume.CoreStoppedDeliberately();
        Assert.Equal(OutputResumeDecision.None, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
    }

    // Owner ruling 2026-10-02: the stream reconnects. A destination left disconnected ends the
    // broadcast, so it goes back out whether or not Zoom came back.
    [Fact]
    public void AStreamIsReconnectedWhateverHappenedToZoom()
    {
        foreach (var outcome in new[] { MediaCoreRecoveryOutcome.ZoomRejoined, MediaCoreRecoveryOutcome.ZoomRejoinFailed, MediaCoreRecoveryOutcome.CoreOnly })
        {
            var resume = new OutputResumeAfterCoreRestart(requiresMeeting: false);
            resume.CoreCrashed(outputWasOn: true);
            Assert.Equal(OutputResumeDecision.Resume, resume.RecoveryCompleted(outcome));
            Assert.Equal(OutputResumeDecision.None, resume.RecoveryCompleted(outcome));
        }
    }

    [Fact]
    public void AStreamTheOperatorStoppedDuringRecoveryStaysStopped()
    {
        var resume = new OutputResumeAfterCoreRestart(requiresMeeting: false);
        resume.CoreCrashed(outputWasOn: true);
        resume.OperatorChose();
        Assert.Equal(OutputResumeDecision.None, resume.RecoveryCompleted(MediaCoreRecoveryOutcome.ZoomRejoined));
    }
}
