namespace CoreVideoPro.MediaCore.Services;

/// <summary>Ephemeral interest in the native Sources input thumbnails.</summary>
public interface ISourcePreviewTransport
{
    Task SetSourcePreviewActiveAsync(bool enabled, CancellationToken cancellationToken = default);
}
