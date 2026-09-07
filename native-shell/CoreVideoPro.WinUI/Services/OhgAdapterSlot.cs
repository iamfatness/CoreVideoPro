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
/// <para><b>Written on the UI thread, read from ANY thread.</b> Writes come from the constructor
/// and <c>ApplyShowConfigAsync</c>, both on the dispatcher, and the apply queue reads it there too
/// — but <c>StudioControlSurface.GetState()</c> also reads it, on an HTTP/OSC transport thread, for
/// the state document's shadow-command field. Hence <see cref="System.Threading.Volatile"/> rather
/// than a plain auto-property: the reference must not be torn or hoisted out of a loop. It is
/// deliberately NOT locked. A reader racing a swap gets either the old or the new adapter, and a
/// state document naming the previous shadowed command for one 150 ms feedback tick is acceptable;
/// a lock here would park a transport thread behind the UI thread for a single reference read.</para>
/// </summary>
public sealed class OhgAdapterSlot
{
    private OhgHostAdapter? _current;

    /// <summary>The adapter host commands apply to right now, or null when OHG is not configured
    /// (the default: no show config on this machine).</summary>
    public OhgHostAdapter? Current => Volatile.Read(ref _current);

    /// <summary>Swap the adapter (UI thread). Commands already enqueued are NOT cancelled — they
    /// run against whatever is current when they reach the head of the queue, which is this one.</summary>
    public void Replace(OhgHostAdapter? adapter) => Volatile.Write(ref _current, adapter);
}
