using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public sealed class MediaBinService
{
    public IReadOnlyList<MediaBinGroup> LoadGroups()
    {
        try
        {
            var assets = MediaBinClassifier.ScanMediaRoots(MediaBinClassifier.ResolveMediaRoots());
            return MediaBinClassifier.GroupAssets(assets)
                .Select(ToMediaBinGroup)
                .ToList();
        }
        catch
        {
            return [];
        }
    }

    private static MediaBinGroup ToMediaBinGroup(MediaBinGroupDescriptor group) =>
        new()
        {
            Kind = group.Kind,
            Label = group.Label,
            Assets = group.Assets.Select(ToMediaAsset).ToList()
        };

    private static MediaAsset ToMediaAsset(MediaBinAssetDescriptor asset) =>
        new()
        {
            Id = asset.Id,
            Name = asset.Name,
            Kind = asset.Kind,
            DurationMs = asset.DurationMs
        };
}