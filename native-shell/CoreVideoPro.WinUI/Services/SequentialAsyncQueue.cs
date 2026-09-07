using System;
using System.Threading;
using System.Threading.Tasks;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// A single-consumer async queue: each enqueued unit of work starts only after the previous one has
/// COMPLETED, awaits included.
///
/// <para><b>Why this exists.</b> OHG host commands arrive from the show engine already numbered in
/// <c>seq</c> order and were each fire-and-forgotten onto the dispatcher
/// (<c>_ = ApplyHostCommandAsync(cmd)</c>). That is ordered only until one of them awaits: <c>cut</c>
/// and <c>auto</c> await <c>TakeAsync</c>, and while that take is in flight the NEXT command's
/// <c>applyLook</c> runs and rewrites the preview draft the take is reading — the guest on air is
/// then the one the engine staged AFTER the take it asked for. Chaining every command through one
/// queue makes "in seq order" true across awaits, not just up to the first one.</para>
///
/// <para><b>Exceptions never break the chain.</b> A link that throws is reported through
/// <c>onError</c> (if given) and swallowed; the next link runs regardless. A queue that stopped
/// draining because one command faulted would silently freeze the whole show's direction.</para>
///
/// <para><b>Thread affinity.</b> Continuations are scheduled on the <see cref="TaskScheduler"/>
/// captured at <see cref="Enqueue"/> time — the UI thread's when enqueued from inside a
/// <c>UiDispatch</c> callback (a <see cref="SynchronizationContext"/> is installed there), and the
/// thread pool otherwise, which is what lets this be tested without a dispatcher. Ordering does not
/// depend on which: it comes from the chain, not the scheduler.</para>
/// </summary>
public sealed class SequentialAsyncQueue
{
    private readonly object _gate = new();
    private readonly Action<Exception>? _onError;

    private Task _tail = Task.CompletedTask;

    public SequentialAsyncQueue(Action<Exception>? onError = null) => _onError = onError;

    /// <summary>Queue <paramref name="work"/> behind everything already queued. The returned task
    /// completes when this link has finished (it never faults — see the type's contract).</summary>
    public Task Enqueue(Func<Task> work)
    {
        if (work is null) throw new ArgumentNullException(nameof(work));

        var scheduler = SynchronizationContext.Current is null
            ? TaskScheduler.Default
            : TaskScheduler.FromCurrentSynchronizationContext();

        lock (_gate)
        {
            // ContinueWith on the PREVIOUS tail — not on a completed task — is the ordering: the
            // link cannot even start until its predecessor's own task (the unwrapped one, so its
            // awaits are included) has completed.
            var next = _tail.ContinueWith(
                _ => RunAsync(work),
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                scheduler).Unwrap();

            _tail = next;
            return next;
        }
    }

    private async Task RunAsync(Func<Task> work)
    {
        try
        {
            await work().ConfigureAwait(true);
        }
        catch (Exception ex)
        {
            _onError?.Invoke(ex);
        }
    }
}
