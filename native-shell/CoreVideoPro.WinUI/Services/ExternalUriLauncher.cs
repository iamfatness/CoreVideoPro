using Windows.System;

namespace CoreVideoPro.WinUI.Services;

public static class ExternalUriLauncher
{
    private static Microsoft.UI.Dispatching.DispatcherQueue? _dispatcher;

    public static void BindDispatcher(Microsoft.UI.Dispatching.DispatcherQueue dispatcher) =>
        _dispatcher = dispatcher;

    public static Task OpenAsync(string url) =>
        _dispatcher is null
            ? LaunchOnCurrentThreadAsync(url)
            : LaunchViaDispatcherAsync(url);

    private static Task LaunchViaDispatcherAsync(string url)
    {
        var completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_dispatcher!.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Normal, () =>
            {
                _ = LaunchOnCurrentThreadAsync(url)
                    .ContinueWith(
                        task => completion.TrySetFromTask(task),
                        TaskScheduler.Default);
            }))
        {
            completion.TrySetException(
                new InvalidOperationException("Could not schedule browser launch on the UI thread."));
        }

        return completion.Task;
    }

    private static async Task LaunchOnCurrentThreadAsync(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri))
        {
            throw new InvalidOperationException("The authorization URL is not valid.");
        }

        LaunchLog.Write($"oauth: opening browser {uri.Host}{uri.AbsolutePath}");

        try
        {
            var launched = await Launcher.LaunchUriAsync(uri);
            if (!launched)
            {
                throw new InvalidOperationException(
                    "Windows could not open your browser. Check the default browser and try again.");
            }

            return;
        }
        catch (Exception ex) when (ex is not InvalidOperationException)
        {
            LaunchLog.Write($"oauth: Launcher.LaunchUriAsync failed ({ex.GetType().Name}): {ex.Message}");
        }

        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = url,
                UseShellExecute = true
            });
            LaunchLog.Write("oauth: opened browser via shell fallback");
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"oauth: shell fallback failed: {ex.Message}");
            throw new InvalidOperationException(
                "Windows could not open your browser. Check the default browser and try again.",
                ex);
        }
    }
}

internal static class TaskCompletionExtensions
{
    internal static void TrySetFromTask(this TaskCompletionSource completion, Task task)
    {
        if (task.IsFaulted)
        {
            completion.TrySetException(task.Exception!.InnerExceptions);
            return;
        }

        if (task.IsCanceled)
        {
            completion.TrySetCanceled();
            return;
        }

        completion.TrySetResult();
    }
}