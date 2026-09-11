namespace CoreVideoPro.WinUI.Services;

/// <summary>What <c>MainWindow.OnAppWindowClosing</c> does with a close request.</summary>
internal enum CloseRequestHandling
{
    /// <summary>Nothing is live (or shutdown already started): run the existing shutdown path.</summary>
    Proceed,

    /// <summary>The guard took the close: it asks, and itself starts the shutdown when appropriate.</summary>
    Guarded,

    /// <summary>The guard is already active (dialog open, or outputs finishing): ignore this request.</summary>
    Ignored
}

/// <summary>
/// The close-guard flow for T1.8 (#461), extracted from <c>MainWindow</c> so it is unit-tested
/// (fix round 1). Every UI effect is injected:
/// <list type="bullet">
/// <item><c>ask</c> shows "Stop outputs and close?" and returns true for "Stop and close". It may
/// throw (no XamlRoot, a second ContentDialog already open, a style failure).</item>
/// <item><c>stopOutputs</c> is <see cref="OutputShutdownCoordinator"/> (bounded, 15 s).</item>
/// <item><c>beginShutdown</c> is the existing, unchanged shutdown path.</item>
/// <item><c>bringToFront</c> restores and activates the window, so a dialog opened in a minimized
/// window, or a close clicked while it is up, is never invisible.</item>
/// </list>
///
/// <para><b>Rulings it encodes.</b> "Keep running" changes nothing. A second close while the
/// guard is active is ignored (never a force-exit — that is exactly what cuts the recording off).
/// And if the dialog cannot be shown while outputs are live, the flow takes the "Stop and close"
/// path (finish the files, then close) — NEVER the old kill path. Only a failure of the stop itself
/// falls through to a plain shutdown, because the stop is already bounded and self-contained.</para>
/// </summary>
internal sealed class CloseGuardFlow
{
    private readonly Func<CloseGuardDecision> _evaluate;
    private readonly Func<CloseGuardDecision, Task<bool>> _ask;
    private readonly Func<CloseGuardDecision, Task<OutputShutdownOutcome>> _stopOutputs;
    private readonly Action _beginShutdown;
    private readonly Action _bringToFront;
    private readonly Action<string> _log;
    private readonly Action<string, Exception> _logException;

    internal CloseGuardFlow(
        Func<CloseGuardDecision> evaluate,
        Func<CloseGuardDecision, Task<bool>> ask,
        Func<CloseGuardDecision, Task<OutputShutdownOutcome>> stopOutputs,
        Action beginShutdown,
        Action bringToFront,
        Action<string> log,
        Action<string, Exception> logException)
    {
        _evaluate = evaluate;
        _ask = ask;
        _stopOutputs = stopOutputs;
        _beginShutdown = beginShutdown;
        _bringToFront = bringToFront;
        _log = log;
        _logException = logException;
    }

    /// <summary>True while the dialog is open or the outputs are finishing.</summary>
    internal bool Active { get; private set; }

    /// <summary>The running guard flow (tests await it).</summary>
    internal Task PendingFlow { get; private set; } = Task.CompletedTask;

    internal CloseRequestHandling HandleCloseRequest(bool shutdownStarted)
    {
        if (Active)
        {
            _log("shutdown: close requested while the close guard is active — ignored");
            TryBringToFront();
            return CloseRequestHandling.Ignored;
        }

        if (shutdownStarted)
        {
            return CloseRequestHandling.Proceed;
        }

        CloseGuardDecision decision;
        try
        {
            decision = _evaluate();
        }
        catch (Exception ex)
        {
            _logException("shutdown: close guard evaluation failed; using the existing shutdown path", ex);
            return CloseRequestHandling.Proceed;
        }

        if (!decision.ShouldAsk)
        {
            return CloseRequestHandling.Proceed;
        }

        Active = true;
        PendingFlow = RunAsync(decision);
        return CloseRequestHandling.Guarded;
    }

    private async Task RunAsync(CloseGuardDecision decision)
    {
        try
        {
            _log($"shutdown: close requested while live ({decision.Description}); asking the operator");
            bool stopAndClose;
            try
            {
                TryBringToFront();
                stopAndClose = await _ask(decision).ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                // Could not ask. Outputs are live, so finish them rather than cut them off.
                _logException("shutdown: could not ask; stopping outputs before closing", ex);
                stopAndClose = true;
            }

            if (!stopAndClose)
            {
                _log("shutdown: operator chose Keep running; the app stays open");
                Active = false;
                return;
            }

            try
            {
                var outcome = await _stopOutputs(decision).ConfigureAwait(true);
                _log($"shutdown: outputs before close: {outcome}");
            }
            catch (Exception ex)
            {
                _logException("shutdown: stopping outputs before close failed; closing anyway", ex);
            }
        }
        catch (Exception ex)
        {
            _logException("shutdown: close guard failed; closing", ex);
        }

        Active = false;
        try
        {
            _beginShutdown();
        }
        catch (Exception ex)
        {
            _logException("shutdown: starting shutdown after the close guard failed", ex);
        }
    }

    private void TryBringToFront()
    {
        try { _bringToFront(); }
        catch (Exception ex) { _logException("shutdown: bringing the window forward failed", ex); }
    }
}
