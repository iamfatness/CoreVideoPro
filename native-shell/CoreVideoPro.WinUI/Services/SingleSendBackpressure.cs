using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The outcome of one single-send media-core sync.
/// </summary>
public enum SingleSendOutcome
{
    Sent,
    SkippedForBackpressure,
    Failed
}

/// <summary>
/// Runs a single-send media-core sync (a discrete operator action, sent once) and makes a skip
/// for backpressure explicit, so it is re-armed and never quietly lost (T1.5 review fix).
///
/// <see cref="MediaCoreSyncInFlightException"/> means the command was NOT delivered: the
/// supervisor has one sync slot, and another sync held it. Two single-send paths used to swallow
/// that on the assumption that "the periodic sync reapplies". With Engine (capture) on that was
/// roughly true, because the spine sync repeats the preview scene and the multiview layout. With
/// Engine off nothing repeats them. Since T1.5 the bridge polls the core every 250 ms with
/// Engine off too, so a collision is now possible in exactly that state. A Preview-scene pick
/// that collided was silently dropped, and the operator could Take a scene the core had never
/// composited in Preview.
///
/// The rule: a skipped single send must re-arm its own delivery. <paramref name="onSkipped"/>
/// hands it to a retry that is guaranteed to run: the production-sync retry worker, or the
/// sender's own debounce. Any other failure goes to <paramref name="onFailed"/>, which is the
/// caller's existing handling.
/// </summary>
public static class SingleSendBackpressure
{
    public static async Task<SingleSendOutcome> RunAsync(
        Func<Task> send,
        Action onSkipped,
        Action<Exception> onFailed)
    {
        ArgumentNullException.ThrowIfNull(send);
        ArgumentNullException.ThrowIfNull(onSkipped);
        ArgumentNullException.ThrowIfNull(onFailed);
        try
        {
            await send().ConfigureAwait(false);
            return SingleSendOutcome.Sent;
        }
        catch (MediaCoreSyncInFlightException)
        {
            onSkipped();
            return SingleSendOutcome.SkippedForBackpressure;
        }
        catch (Exception ex)
        {
            onFailed(ex);
            return SingleSendOutcome.Failed;
        }
    }
}
