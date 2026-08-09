using System;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI;

/// <summary>
/// The ONLY sanctioned way to run a callback on the UI dispatcher.
///
/// WHY (2026-08-09, three crashes in one live meeting): an exception thrown
/// inside a <see cref="DispatcherQueue.TryEnqueue(DispatcherQueueHandler)"/>
/// callback does NOT reach <c>Application.UnhandledException</c> or the
/// AppDomain handler — WinUI 3 stows the failure HRESULT and fail-fasts with
/// 0xc000027b (CoreMessagingXP!DispatcherQueue::DeferInvokeCallback), leaving
/// NO managed stack anywhere. The day's dumps decode as ordinary managed bugs
/// that would have been one-line log entries anywhere else:
///   0x80004003 (E_POINTER)  = NullReferenceException in a queued callback
///   0x8000000b (E_BOUNDS)   = ArgumentOutOfRangeException in a queued callback
///
/// Policy: LOG LOUDLY, KEEP THE SHOW RUNNING. A skipped UI update is a cosmetic
/// glitch; a fail-fast ends a live production. Every catch here is a bug to fix
/// — the log line carries the full stack and a caller tag so it is a one-line
/// diagnosis instead of a 1GB dump autopsy.
/// </summary>
internal static class UiDispatch
{
    /// <summary>Run <paramref name="action"/> on the dispatcher (inline when
    /// already on the UI thread), never letting an exception escape into the
    /// dispatcher's fail-fast path.</summary>
    public static void Run(DispatcherQueue? dispatcher, Action action, string tag)
    {
        if (dispatcher is null || dispatcher.HasThreadAccess)
        {
            Invoke(action, tag);
            return;
        }

        var enqueued = dispatcher.TryEnqueue(() => Invoke(action, tag));
        if (!enqueued)
        {
            // Dispatcher shutting down (app close): dropping the update is correct.
            LaunchLog.Write($"ui-dispatch: enqueue refused (shutdown?) tag={tag}");
        }
    }

    /// <summary>Priority variant of <see cref="Run(DispatcherQueue?, Action, string)"/>.
    /// Never runs inline — the callers that pass a priority use the enqueue itself
    /// as a coalescing gate (scheduled-flag patterns).</summary>
    public static void Enqueue(DispatcherQueue dispatcher, DispatcherQueuePriority priority, Action action, string tag)
    {
        var enqueued = dispatcher.TryEnqueue(priority, () => Invoke(action, tag));
        if (!enqueued)
        {
            LaunchLog.Write($"ui-dispatch: enqueue refused (shutdown?) tag={tag}");
        }
    }

    private static void Invoke(Action action, string tag)
    {
        try
        {
            action();
        }
        catch (Exception ex)
        {
            // This line replaces a process-killing 0xc000027b fail-fast. If it
            // appears in the log, there IS a real bug at `tag` — fix it; do not
            // treat the catch as the fix.
            LaunchLog.Write($"ui-dispatch: CALLBACK THREW (crash averted) tag={tag} :: {ex}");
        }
    }
}
