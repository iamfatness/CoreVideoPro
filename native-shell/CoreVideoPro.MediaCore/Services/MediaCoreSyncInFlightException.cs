namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Thrown when a media-core sync is requested while another sync is already in
/// flight. This is a transient backpressure signal, not a hard failure: the
/// caller should keep its requested state and let a later sync apply it rather
/// than reporting an error to the operator.
/// </summary>
/// <remarks>
/// Derives from <see cref="InvalidOperationException"/> so existing call sites
/// that already swallow background sync errors keep working unchanged; callers
/// that care about backpressure (e.g. the explicit Stream/Record toggles) can
/// catch this type specifically and retry.
/// </remarks>
public sealed class MediaCoreSyncInFlightException : InvalidOperationException
{
    public MediaCoreSyncInFlightException()
        : base("media-core sync in flight; skipped for backpressure")
    {
    }
}
