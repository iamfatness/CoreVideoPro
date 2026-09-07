namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The mutable holder for the OHG host adapter the control surface applies host commands through
/// (Plan 7b Task 10). It exists so <c>StudioControlSurface.ReplaceOhgAdapter</c> has a seam a unit
/// test can drive without a <c>DispatcherQueue</c>.
///
/// <para><b>Read at APPLY time, never at enqueue time.</b> Host commands run through a
/// <c>SequentialAsyncQueue</c> and a link can sit queued across an <c>await</c> — a take, say —
/// while the operator saves a new show config in Settings. The controller ruling is that an
/// in-flight link uses whichever adapter is current when it RUNS: the engine has by then been
/// restarted onto the new config, so applying the OLD look→scene presets would cue a scene the
/// engine no longer means. Capturing the adapter into a local at enqueue time reintroduces exactly
/// that bug, which is why <c>ApplyHostCommandAsync</c> reads <see cref="Current"/> inside its own
/// body.</para>
///
/// <para>UI-THREAD ONLY. Both the reads (from the queue's links) and the writes (from
/// <c>ApplyShowConfigAsync</c>) happen on the dispatcher, so no lock is needed and none is taken —
/// a lock here would only make it look safe to call from somewhere it is not.</para>
/// </summary>
public sealed class OhgAdapterSlot
{
    /// <summary>The adapter host commands apply to right now, or null when OHG is not configured
    /// (the default: no show config on this machine).</summary>
    public OhgHostAdapter? Current { get; private set; }

    /// <summary>Swap the adapter. Commands already enqueued are NOT cancelled — they run against
    /// whatever is current when they reach the head of the queue, which is this one.</summary>
    public void Replace(OhgHostAdapter? adapter) => Current = adapter;
}
