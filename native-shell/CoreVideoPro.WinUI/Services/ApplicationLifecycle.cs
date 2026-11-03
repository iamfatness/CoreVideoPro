namespace CoreVideoPro.WinUI.Services;

internal static class ApplicationLifecycle
{
    private static AppActivationCoordinator? _activation;
    private static readonly ExitOnce s_forceExit = new();

    internal static void BindActivation(AppActivationCoordinator activation) => _activation = activation;

    internal static void PrepareShutdown()
    {
        _activation?.SetActivationHandler(null);
        try { _activation?.Unsubscribe(); }
        catch (Exception ex) { LaunchLog.WriteException("shutdown: activation unsubscribe failed", ex); }
    }

    /// <summary>The fallback exit: a failed or timed-out cleanup, the shutdown watchdog, or a
    /// second close while cleanup is running. Safe from any thread; never touches XAML.</summary>
    internal static void ForceExit(int exitCode = 0) =>
        ForceExit(
            exitCode,
            s_forceExit,
            LaunchLog.Write,
            PrepareShutdown,
            ShutdownCompletion.TerminateCurrentProcess,
            Environment.Exit);

    /// <summary>Runs once: write the log line, unhook activation, then end the process with
    /// <c>TerminateProcess</c>. Each step is injected so the order can be tested without ending
    /// the test host.
    ///
    /// <para>#457 follow-up: this used to end in <c>Environment.Exit</c>. That is
    /// <c>ExitProcess</c>, which runs <c>DLL_PROCESS_DETACH</c> in Microsoft.UI.Xaml and
    /// CoreMessagingXP, the components whose teardown fail-fasted (0xc000027b) in #457, here in an
    /// even less defined state because cleanup did not finish. <c>TerminateProcess</c> runs no
    /// further user-mode code, the same exit the clean path takes (see
    /// <see cref="ShutdownCompletion"/>). The log line is written synchronously first, so it is on
    /// disk. <c>Environment.Exit</c> stays only as the fallback if the terminate call returns.</para>
    /// </summary>
    internal static void ForceExit(
        int exitCode,
        ExitOnce once,
        Action<string> log,
        Action prepareShutdown,
        Action<uint> terminateProcess,
        Action<int> exitProcessFallback)
    {
        if (!once.TryClaim())
        {
            return;
        }

        try
        {
            log($"shutdown: force exit code={exitCode}");
            prepareShutdown();
        }
        finally
        {
            terminateProcess(unchecked((uint)exitCode));
            // Unreachable when the terminate call succeeds.
            try { log("shutdown: TerminateProcess returned; falling back to Environment.Exit"); }
            catch { /* the exit below must still run */ }
            exitProcessFallback(exitCode);
        }
    }

    /// <summary>A run-once latch, so a watchdog and a second close cannot both exit.</summary>
    internal sealed class ExitOnce
    {
        private int _claimed;

        internal bool TryClaim() => Interlocked.Exchange(ref _claimed, 1) == 0;
    }
}
