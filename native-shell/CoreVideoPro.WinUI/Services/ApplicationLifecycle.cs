namespace CoreVideoPro.WinUI.Services;

internal static class ApplicationLifecycle
{
    private static AppActivationCoordinator? _activation;
    private static int _exitRequested;

    internal static void BindActivation(AppActivationCoordinator activation) => _activation = activation;

    internal static void PrepareShutdown()
    {
        _activation?.Unsubscribe();
        _activation?.SetActivationHandler(null);
    }

    internal static void ForceExit(int exitCode = 0)
    {
        if (Interlocked.Exchange(ref _exitRequested, 1) == 1)
        {
            return;
        }

        LaunchLog.Write($"shutdown: force exit code={exitCode}");
        PrepareShutdown();

        try
        {
            Microsoft.UI.Xaml.Application.Current.Exit();
        }
        catch
        {
            // Best effort.
        }

        Environment.Exit(exitCode);
    }
}