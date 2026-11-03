using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// #457 follow-up: the ForceExit fallback (cleanup failed or timed out, the shutdown watchdog, or a
/// second close during cleanup) ends the process with TerminateProcess, like the clean path, instead
/// of Environment.Exit. Environment.Exit ends in ExitProcess, which runs DLL_PROCESS_DETACH in
/// Microsoft.UI.Xaml and CoreMessagingXP, the components whose teardown fail-fasted in #457.
/// Process exit cannot be unit-tested; these tests pin the order and the run-once gate.
/// </summary>
public sealed class ApplicationLifecycleForceExitTests
{
    private sealed class Recorder
    {
        public List<string> Steps { get; } = [];
        public uint? TerminateCode { get; private set; }
        public bool TerminateReturns { get; init; }
        public bool PrepareThrows { get; init; }
        public ApplicationLifecycle.ExitOnce Once { get; } = new();

        public void Run(int exitCode = 0) =>
            ApplicationLifecycle.ForceExit(
                exitCode,
                Once,
                log: message => Steps.Add("log:" + message),
                prepareShutdown: () =>
                {
                    Steps.Add("prepare");
                    if (PrepareThrows) throw new InvalidOperationException("unsubscribe failed");
                },
                terminateProcess: code =>
                {
                    Steps.Add("terminate");
                    TerminateCode = code;
                    if (!TerminateReturns) throw new TerminatedSignal();
                },
                exitProcessFallback: code => Steps.Add($"exit:{code}"));
    }

    // Stands in for "the process is gone": nothing after a successful terminate may run.
    private sealed class TerminatedSignal : Exception;

    [Fact]
    public void TheLogLineIsWrittenBeforeTheProcessIsTerminated()
    {
        var recorder = new Recorder();

        Assert.Throws<TerminatedSignal>(() => recorder.Run());

        Assert.Equal(3, recorder.Steps.Count);
        Assert.Equal("log:shutdown: force exit code=0", recorder.Steps[0]);
        Assert.Equal("prepare", recorder.Steps[1]);
        Assert.Equal("terminate", recorder.Steps[2]);
        Assert.Equal(0u, recorder.TerminateCode);
        Assert.DoesNotContain(recorder.Steps, step => step.StartsWith("exit:", StringComparison.Ordinal));
    }

    [Fact]
    public void ASecondCallIsANoOp()
    {
        var recorder = new Recorder { TerminateReturns = true };

        recorder.Run();
        var afterFirst = recorder.Steps.Count;
        recorder.Run();

        Assert.Equal(afterFirst, recorder.Steps.Count);
        Assert.Single(recorder.Steps, "terminate");
    }

    [Fact]
    public void AFailingPrepareStillTerminates()
    {
        var recorder = new Recorder { PrepareThrows = true };

        Assert.Throws<TerminatedSignal>(() => recorder.Run());

        Assert.Equal("terminate", recorder.Steps[^1]);
    }

    [Fact]
    public void ATerminateThatReturnsFallsBackToExitProcessWithTheSameCode()
    {
        var recorder = new Recorder { TerminateReturns = true };

        recorder.Run(exitCode: 3);

        Assert.Equal(3u, recorder.TerminateCode);
        Assert.Contains(recorder.Steps, step => step.StartsWith("log:shutdown: TerminateProcess returned", StringComparison.Ordinal));
        Assert.Equal("exit:3", recorder.Steps[^1]);
    }
}
