namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Which mixer strips are shown. Zoom guests follow <see cref="ZoomSourceSetPolicy"/>
/// (the same audio source set as spine subscribe). Non-Zoom PCM sources
/// (zoom-mix, local capture, media, capture devices) always stay.
/// </summary>
public static class MixerChannelSetPolicy
{
    public static bool IsZoomParticipantChannel(string sourceId)
    {
        if (string.IsNullOrWhiteSpace(sourceId) || sourceId.Contains(':', StringComparison.Ordinal))
        {
            return false;
        }

        return sourceId is not "zoom-mix" and not "local-machine-audio" and not "media";
    }

    public static bool DisplayOnMixer(string sourceId, IReadOnlySet<string> zoomSourceParticipantIds)
    {
        if (!IsZoomParticipantChannel(sourceId))
        {
            return true;
        }

        return zoomSourceParticipantIds.Contains(sourceId);
    }
}
