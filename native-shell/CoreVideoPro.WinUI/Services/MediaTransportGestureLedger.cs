namespace CoreVideoPro.WinUI.Services;

/// <summary>What a skipped <c>set-media-transport</c> gesture should do next.</summary>
public enum MediaTransportGestureStep
{
    /// <summary>A newer tap for this asset claimed the slot; this gesture must stop, silently.</summary>
    Superseded,

    /// <summary>Re-arm: send the same gesture again after the backoff.</summary>
    Retry,

    /// <summary>Out of attempts; release the slot and tell the operator.</summary>
    GiveUp
}

/// <summary>
/// LATEST WINS, per media asset, for the one-shot <c>set-media-transport</c> gesture
/// (#535 slice 3b). A tap CLAIMS its asset's slot with a fresh token; any attempt still
/// carrying an older token is superseded and must not reach the core.
///
/// The whole decision lives here rather than as a predicate at the call site, for the reason
/// the first cut of this fix was wrong: it gated only the RETRY SCHEDULING, so a retry that had
/// already been scheduled before it was superseded still sent. Sequence that broke it —
/// tap 1 (pause, token 1) is skipped for backpressure and schedules a 120 ms retry; tap 2
/// (resume, token 2) lands and completes, freeing the slot; tap 1's delayed retry then fires
/// against an empty slot and delivers the stale PAUSE. The clip is paused on air and the
/// production sync deliberately no longer re-asserts play state, so nothing corrects it.
/// <see cref="ShouldSend"/> is therefore consulted immediately before every send, attempt 0
/// included, and <see cref="Release"/> only clears a slot the caller still owns.
///
/// It is NOT thread-safe by design: every caller runs on the UI thread (the tap itself, or a
/// <c>RunOnUiThread</c> body). A retry's post-delay continuation lands on the thread pool, so it
/// must marshal back before asking.
/// </summary>
public sealed class MediaTransportGestureLedger
{
    public const int DefaultRetryAttempts = 5;

    private readonly Dictionary<string, long> _current = new(StringComparer.Ordinal);
    private readonly int _retryAttempts;
    private long _sequence;

    public MediaTransportGestureLedger(int retryAttempts = DefaultRetryAttempts)
    {
        _retryAttempts = retryAttempts;
    }

    /// <summary>Claims the asset's slot for a new tap and returns that tap's token.</summary>
    public long Claim(string mediaAssetId)
    {
        var token = ++_sequence;
        _current[mediaAssetId] = token;
        return token;
    }

    /// <summary>
    /// May this attempt still reach the core? False once a newer tap for the same asset has
    /// claimed the slot, or once the gesture has been released (completed or given up).
    /// </summary>
    public bool ShouldSend(string mediaAssetId, long token) =>
        _current.TryGetValue(mediaAssetId, out var current) && current == token;

    /// <summary>
    /// The backpressure gate skipped this attempt — the command was NOT delivered. Decides
    /// whether to re-arm, give up (releasing the slot), or stop because a newer tap won.
    /// </summary>
    public MediaTransportGestureStep OnSkipped(string mediaAssetId, long token, int attempt)
    {
        if (!ShouldSend(mediaAssetId, token))
        {
            return MediaTransportGestureStep.Superseded;
        }

        if (attempt >= _retryAttempts)
        {
            _current.Remove(mediaAssetId);
            return MediaTransportGestureStep.GiveUp;
        }

        return MediaTransportGestureStep.Retry;
    }

    /// <summary>
    /// Releases the slot IF this gesture still owns it. Returns true when it did — which is
    /// also the test for "am I still the gesture the operator is waiting on", so a superseded
    /// attempt's completion or failure never writes an operator-facing status.
    /// </summary>
    public bool Release(string mediaAssetId, long token)
    {
        if (!ShouldSend(mediaAssetId, token))
        {
            return false;
        }

        _current.Remove(mediaAssetId);
        return true;
    }
}
