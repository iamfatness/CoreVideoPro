using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private static SrtIngestSource CreateSrtIngestSource(int number) => new()
    {
        Id = $"srt-source-{number:00}",
        Number = number,
        Port = (10000 + number - 1).ToString()
    };

    private IReadOnlyList<MediaCoreSrtIngestSourceWire> BuildSrtIngestSourceSettings() =>
        SrtIngestSources
            .Select(source => new MediaCoreSrtIngestSourceWire(
                source.Id,
                source.DeviceId,
                source.Name,
                NormalizeOutputText(source.Mode, "listener"),
                NormalizeOutputText(source.Host, "0.0.0.0"),
                ParsePositiveInt(source.Port) ?? 10000,
                ParsePositiveInt(source.LatencyMs) ?? 120,
                NormalizeOptionalOutputText(source.StreamId),
                string.IsNullOrWhiteSpace(source.Passphrase) ? null : source.Passphrase))
            .ToList();
}
