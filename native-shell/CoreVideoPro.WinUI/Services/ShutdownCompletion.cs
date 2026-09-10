using System.Runtime.InteropServices;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// How the process ends once <c>MainWindow.ShutdownAsync</c> has finished (T1.7, #457).
///
/// <para><b>The crash this avoids.</b> The normal close path used to call <c>Close()</c> and then
/// <c>Application.Current.Exit()</c>, which hands the process back to XAML.
/// <c>FrameworkApplication::StartDesktop</c> then runs
/// <c>DispatcherQueueController::ShutdownQueue</c>, which drains every DispatcherQueue work item and
/// timer still pending. A leftover item runs against torn-down XAML and returns a failure HRESULT.
/// CoreMessaging fail-fasts on that: 0xc000027b in CoreMessagingXP.dll from
/// <c>DispatcherQueue::DeferInvokeCallback</c>, below <c>ShutdownQueue</c> on the UI thread. It hit
/// 2 of the last 10 graceful closes, both after long in-meeting sessions. The failing items were a
/// <c>DispatcherQueueTimer::TimerCallback</c> (stowed E_UNEXPECTED, dump 48644) and a non-managed
/// callback (stowed E_ABORT, dump 17304). Both were after "shutdown: resources released", so no air
/// or data was at risk. The cost was a 1.1 GB dump, a WER APPCRASH, and a crash-report prompt on
/// the next launch.</para>
///
/// <para><b>Why TerminateProcess, not Environment.Exit.</b> Both skip the drain, because neither
/// returns to <c>StartDesktop</c>. <c>Environment.Exit</c> ends in <c>ExitProcess</c>, which kills
/// the other threads mid-flight and then runs <c>DLL_PROCESS_DETACH</c> in every loaded module.
/// That includes Microsoft.UI.Xaml and CoreMessagingXP, the two components whose teardown just
/// fail-fasted, now running in an even less defined state. <c>TerminateProcess</c> on our own
/// process runs no further user-mode code at all. It gives up two things, and neither is used:
/// AppDomain.ProcessExit handlers (the app registers none) and unflushed buffered writers (the
/// diagnostic logs go through <c>BoundedLogFile.Append</c>, which writes synchronously, so every
/// line has reached disk before its <c>Write</c> returns).</para>
///
/// <para><b>Only after a clean shutdown.</b> The hard exit is taken only when cleanup finished
/// (media core stopped off the UI thread, control server stopped, view model disposed) AND the
/// window closed. Any failure or timeout keeps the existing <c>ApplicationLifecycle.ForceExit</c>
/// fallback, which already logs why. Session-end telemetry was fired at the top of
/// <c>ShutdownAsync</c> and is fire-and-forget by contract (spec S3.4), exactly as before.</para>
/// </summary>
internal static class ShutdownCompletion
{
    internal enum ExitPath
    {
        // Cleanup and Close both succeeded: stop leftover UI timers, write the last log line,
        // terminate the process directly.
        TerminateAfterCleanShutdown,

        // Cleanup failed, timed out, or Close threw: the existing ForceExit fallback.
        ExistingForceExitFallback
    }

    internal static ExitPath Decide(bool cleanupSucceeded, bool windowClosed) =>
        cleanupSucceeded && windowClosed ? ExitPath.TerminateAfterCleanShutdown : ExitPath.ExistingForceExitFallback;

    /// <summary>Runs the final steps in order: stop timers, write the last log line, then exit.
    /// Each step is injected so the ordering can be tested without ending the test host.</summary>
    internal static ExitPath Complete(
        bool cleanupSucceeded,
        bool windowClosed,
        Action stopLeftoverUiTimers,
        Action<string> log,
        Action<uint> terminateProcess,
        Action forceExitFallback)
    {
        var path = Decide(cleanupSucceeded, windowClosed);
        if (path == ExitPath.ExistingForceExitFallback)
        {
            log($"shutdown: cleanup did not complete (cleanupSucceeded={cleanupSucceeded} windowClosed={windowClosed}); using the force-exit fallback");
            forceExitFallback();
            return path;
        }

        // Defence in depth only: once the process is terminated no timer can fire. This matters
        // if the terminate call itself fails and control falls back to the dispatcher.
        try { stopLeftoverUiTimers(); }
        catch (Exception ex) { log($"shutdown: stopping leftover UI timers failed ({ex.GetType().Name}: {ex.Message})"); }

        log("shutdown: cleanup complete; terminating the process directly (skips the WinUI dispatcher drain, T1.7)");
        terminateProcess(0);
        // Unreachable when the terminate call succeeds. If it returned, do not leave a
        // half-closed process: fall back to the existing path.
        log("shutdown: TerminateProcess returned; using the force-exit fallback");
        forceExitFallback();
        return path;
    }

    /// <summary>The production terminate: ends this process with no further user-mode code.</summary>
    internal static void TerminateCurrentProcess(uint exitCode) =>
        _ = TerminateProcess(GetCurrentProcess(), exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(IntPtr hProcess, uint uExitCode);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();
}
