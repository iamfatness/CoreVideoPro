using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// T1.8 fix round 1 (finding 2): the MainWindow close flow, extracted so every branch is tested.
/// Binding ruling: a dialog failure while outputs are live takes the Stop-and-close path, never
/// the old kill path.
/// </summary>
public sealed class CloseGuardFlowTests
{
    private static readonly CloseGuardDecision Live = new(
        true, "Recording", true, false, 0, OutputStopBaseline.None with { RecordingExpected = true });

    private sealed class Rig
    {
        public CloseGuardDecision Decision = Live;
        public Func<CloseGuardDecision, Task<bool>> Ask = _ => Task.FromResult(true);
        public Func<CloseGuardDecision, Task<OutputShutdownOutcome>>? Stop;
        public readonly List<string> Calls = [];
        public readonly List<string> Log = [];

        public CloseGuardFlow Build() => new(
            evaluate: () => Decision,
            ask: decision => { Calls.Add("ask"); return Ask(decision); },
            stopOutputs: decision =>
            {
                Calls.Add("stop");
                return Stop?.Invoke(decision) ?? Task.FromResult(OutputShutdownOutcome.Finished);
            },
            beginShutdown: () => Calls.Add("shutdown"),
            bringToFront: () => Calls.Add("front"),
            log: Log.Add,
            logException: (message, error) => Log.Add($"{message}: {error.Message}"));
    }

    [Fact]
    public void NothingLiveProceedsWithTheExistingShutdownPath()
    {
        var rig = new Rig { Decision = CloseGuardDecision.Nothing };

        var handling = rig.Build().HandleCloseRequest(shutdownStarted: false);

        Assert.Equal(CloseRequestHandling.Proceed, handling);
        Assert.Empty(rig.Calls);
    }

    [Fact]
    public async Task StopAndCloseStopsThenShutsDown()
    {
        var rig = new Rig();
        var flow = rig.Build();

        Assert.Equal(CloseRequestHandling.Guarded, flow.HandleCloseRequest(false));
        await flow.PendingFlow;

        Assert.Equal(["front", "ask", "stop", "shutdown"], rig.Calls);
        Assert.False(flow.Active);
    }

    [Fact]
    public async Task KeepRunningResetsTheGuardAndDoesNotShutDown()
    {
        var rig = new Rig { Ask = _ => Task.FromResult(false) };
        var flow = rig.Build();

        flow.HandleCloseRequest(false);
        await flow.PendingFlow;

        Assert.Equal(["front", "ask"], rig.Calls);
        Assert.False(flow.Active);
        // The guard is armed again for the next close.
        Assert.Equal(CloseRequestHandling.Guarded, flow.HandleCloseRequest(false));
    }

    [Fact]
    public async Task ADialogFailureStillFinishesTheFilesBeforeClosing()
    {
        var rig = new Rig { Ask = _ => throw new InvalidOperationException("Only a single ContentDialog can be open at any time.") };
        var flow = rig.Build();

        flow.HandleCloseRequest(false);
        await flow.PendingFlow;

        Assert.Equal(["front", "ask", "stop", "shutdown"], rig.Calls);
        Assert.Contains(rig.Log, line => line.StartsWith("shutdown: could not ask; stopping outputs before closing", StringComparison.Ordinal));
    }

    [Fact]
    public async Task AFaultedDialogTaskAlsoTakesTheStopPath()
    {
        var rig = new Rig { Ask = _ => Task.FromException<bool>(new InvalidOperationException("no XamlRoot")) };
        var flow = rig.Build();

        flow.HandleCloseRequest(false);
        await flow.PendingFlow;

        Assert.Equal(["front", "ask", "stop", "shutdown"], rig.Calls);
    }

    [Fact]
    public async Task AStopFailureStillShutsDown()
    {
        var rig = new Rig { Stop = _ => throw new InvalidOperationException("boom") };
        var flow = rig.Build();

        flow.HandleCloseRequest(false);
        await flow.PendingFlow;

        Assert.Equal(["front", "ask", "stop", "shutdown"], rig.Calls);
        Assert.False(flow.Active);
    }

    [Fact]
    public async Task ASecondCloseWhileActiveIsIgnoredAndBringsTheWindowForward()
    {
        var answer = new TaskCompletionSource<bool>();
        var rig = new Rig { Ask = _ => answer.Task };
        var flow = rig.Build();

        flow.HandleCloseRequest(false);
        Assert.True(flow.Active);
        Assert.Equal(CloseRequestHandling.Ignored, flow.HandleCloseRequest(false));
        Assert.Equal(CloseRequestHandling.Ignored, flow.HandleCloseRequest(false));

        answer.SetResult(false);
        await flow.PendingFlow;

        Assert.Equal(["front", "ask", "front", "front"], rig.Calls);
        Assert.DoesNotContain("shutdown", rig.Calls);
    }

    [Fact]
    public async Task ASecondCloseWhileOutputsFinishIsIgnored()
    {
        var finishing = new TaskCompletionSource<OutputShutdownOutcome>();
        var rig = new Rig { Stop = _ => finishing.Task };
        var flow = rig.Build();

        flow.HandleCloseRequest(false);
        Assert.Equal(CloseRequestHandling.Ignored, flow.HandleCloseRequest(false));
        Assert.DoesNotContain("shutdown", rig.Calls);

        finishing.SetResult(OutputShutdownOutcome.Finished);
        await flow.PendingFlow;

        Assert.Equal("shutdown", rig.Calls[^1]);
    }

    [Fact]
    public void AnEvaluationFailureFallsBackToTheExistingPath()
    {
        var flow = new CloseGuardFlow(
            evaluate: () => throw new InvalidOperationException("boom"),
            ask: _ => Task.FromResult(true),
            stopOutputs: _ => Task.FromResult(OutputShutdownOutcome.Finished),
            beginShutdown: () => { },
            bringToFront: () => { },
            log: _ => { },
            logException: (_, _) => { });

        Assert.Equal(CloseRequestHandling.Proceed, flow.HandleCloseRequest(false));
    }

    [Fact]
    public void OnceShutdownStartedTheGuardStepsAside()
    {
        var rig = new Rig();

        Assert.Equal(CloseRequestHandling.Proceed, rig.Build().HandleCloseRequest(shutdownStarted: true));
        Assert.Empty(rig.Calls);
    }
}
