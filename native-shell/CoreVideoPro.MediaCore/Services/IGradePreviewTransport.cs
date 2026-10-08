using CoreVideoPro.MediaCore.Models;
namespace CoreVideoPro.MediaCore.Services;
/// <summary>Control-only transport for leased native editor monitors.</summary>
public interface IGradePreviewTransport
{
    event Action<GradePreviewObservation>? GradePreviewReceived;
    event Action<MediaCoreHealth>? HealthChanged;
    Task SetGradePreviewAsync(string instanceId, string sourceId, long revision, bool enabled,
        MediaCoreColorGradeWire grade, CancellationToken cancellationToken = default);
}
