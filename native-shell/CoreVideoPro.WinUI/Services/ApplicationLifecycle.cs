namespace CoreVideoPro.WinUI.Services;

internal static class ApplicationLifecycle
{
    private static AppActivationCoordinator? _activation;
    private static int _exitRequested;

    internal static void BindActivation(AppActivationCoordinator activation) => _activation = activation;

    internal static void PrepareShutdown()
    {
        _activation?.SetActivationHandler(null);
        try { _activation?.Unsubscribe(); }
        catch (Exception ex) { LaunchLog.WriteException("shutdown: activation unsubscribe failed", ex); }
    }

    internal static void ForceExit(int exitCode = 0)
    {
        if (Interlocked.Exchange(ref _exitRequested, 1) == 1)
        {
            return;
        }

        try
        {
            LaunchLog.Write($"shutdown: force exit code={exitCode}");
            PrepareShutdown();
        }
        finally
        {
            // The watchdog calls from a ThreadPool thread. Application.Current
            // and Exit are XAML-affine; normal Close already runs on the UI
            // thread. This final fallback must never touch XAML or await it.
            Environment.Exit(exitCode);
        }
    }
}
