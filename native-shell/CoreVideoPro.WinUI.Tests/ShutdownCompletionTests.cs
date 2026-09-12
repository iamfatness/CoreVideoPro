using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.7 (#457): after a clean shutdown the process ends directly instead of handing it back to
/// WinUI, whose post-Exit dispatcher drain fail-fasted (0xc000027b) on 2 of 10 closes. Process
/// exit itself cannot be unit-tested. These tests pin the ordering and the gate. The real proof
/// is the scripted close-cycle validation (see the T1.7 report).
/// </summary>
public sealed class ShutdownCompletionTests
{
    private sealed class Recorder
    {
        public List<string> Steps { get; } = [];
        public uint? ExitCode { get; private set; }
        public bool ThrowOnTimers { get; init; }
        public bool TerminateReturns { get; init; }

        public ShutdownCompletion.ExitPath Run(bool cleanupSucceeded, bool windowClosed) =>
            ShutdownCompletion.Complete(
                cleanupSucceeded,
                windowClosed,
                stopLeftoverUiTimers: () =>
                {
                    Steps.Add("timers");
                    if (ThrowOnTimers) throw new InvalidOperationException("timer already closed");
                },
                log: message => Steps.Add("log:" + message),
                terminateProcess: code =>
                {
                    Steps.Add("terminate");
                    ExitCode = code;
                    if (!TerminateReturns) throw new TerminatedSignal();
                },
                forceExitFallback: () => Steps.Add("fallback"));
    }

    // Stands in for "the process is gone": nothing after a successful terminate may run.
    private sealed class TerminatedSignal : Exception;

    [Fact]
    public void ACleanShutdownStopsTimersWritesTheLastLineThenTerminates()
    {
        var recorder = new Recorder();

        Assert.Throws<TerminatedSignal>(() => recorder.Run(cleanupSucceeded: true, windowClosed: true));

        Assert.Equal(3, recorder.Steps.Count);
        Assert.Equal("timers", recorder.Steps[0]);
        Assert.StartsWith("log:shutdown: cleanup complete; terminating the process directly", recorder.Steps[1]);
        Assert.Equal("terminate", recorder.Steps[2]);
        Assert.Equal(0u, recorder.ExitCode);
        Assert.DoesNotContain("fallback", recorder.Steps);
    }

    [Theory]
    [InlineData(false, true)]   // cleanup threw or timed out
    [InlineData(true, false)]   // Close() threw
    [InlineData(false, false)]
    public void AnIncompleteShutdownNeverHardExitsAndKeepsTheFallback(bool cleanupSucceeded, bool windowClosed)
    {
        var recorder = new Recorder();

        var path = recorder.Run(cleanupSucceeded, windowClosed);

        Assert.Equal(ShutdownCompletion.ExitPath.ExistingForceExitFallback, path);
        Assert.DoesNotContain("terminate", recorder.Steps);
        Assert.DoesNotContain("timers", recorder.Steps);
        Assert.Equal("fallback", recorder.Steps[^1]);
        Assert.Contains(recorder.Steps, step => step.StartsWith("log:shutdown: cleanup did not complete", StringComparison.Ordinal));
    }

    [Fact]
    public void AThrowingTimerStopStillTerminates()
    {
        var recorder = new Recorder { ThrowOnTimers = true };

        Assert.Throws<TerminatedSignal>(() => recorder.Run(cleanupSucceeded: true, windowClosed: true));

        Assert.Equal("terminate", recorder.Steps[^1]);
        Assert.Contains(recorder.Steps, step => step.StartsWith("log:shutdown: stopping leftover UI timers failed", StringComparison.Ordinal));
    }

    [Fact]
    public void ATerminateThatReturnsFallsBackRatherThanLeavingAHalfClosedProcess()
    {
        var recorder = new Recorder { TerminateReturns = true };

        var path = recorder.Run(cleanupSucceeded: true, windowClosed: true);

        Assert.Equal(ShutdownCompletion.ExitPath.TerminateAfterCleanShutdown, path);
        Assert.Equal("fallback", recorder.Steps[^1]);
    }
}
