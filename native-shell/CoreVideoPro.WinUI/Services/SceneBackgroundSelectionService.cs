using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class SceneBackgroundSelectionService
{
    public const string NoBackgroundValue = "__none";

    public sealed record SelectionResult(string SelectedAssetId, string? AssetName, bool HasBackground);

    public static string ResolveSelectedAssetId(
        string sceneId,
        IReadOnlyDictionary<string, string> sceneBackgroundAssetIds,
        Func<string, MediaAsset?> findMediaAsset,
        Func<MediaAsset, bool> isVisualMediaAsset)
    {
        if (!sceneBackgroundAssetIds.TryGetValue(sceneId, out var assetId))
        {
            return NoBackgroundValue;
        }

        var asset = findMediaAsset(assetId);
        return asset is not null && isVisualMediaAsset(asset)
            ? asset.Id
            : NoBackgroundValue;
    }

    public static SelectionResult ApplySelection(
        string sceneId,
        string? selectedAssetId,
        IDictionary<string, string> sceneBackgroundAssetIds,
        Func<string, MediaAsset?> findMediaAsset,
        Func<MediaAsset, bool> isVisualMediaAsset)
    {
        if (string.IsNullOrWhiteSpace(selectedAssetId) ||
            string.Equals(selectedAssetId, NoBackgroundValue, StringComparison.Ordinal))
        {
            sceneBackgroundAssetIds.Remove(sceneId);
            return new SelectionResult(NoBackgroundValue, null, HasBackground: false);
        }

        var asset = findMediaAsset(selectedAssetId);
        if (asset is null || !isVisualMediaAsset(asset))
        {
            sceneBackgroundAssetIds.Remove(sceneId);
            return new SelectionResult(NoBackgroundValue, null, HasBackground: false);
        }

        sceneBackgroundAssetIds[sceneId] = asset.Id;
        return new SelectionResult(asset.Id, asset.Name, HasBackground: true);
    }

    public static void PruneMissingBackgrounds(
        IDictionary<string, string> sceneBackgroundAssetIds,
        Func<string, MediaAsset?> findMediaAsset,
        Func<MediaAsset, bool> isVisualMediaAsset)
    {
        foreach (var pair in sceneBackgroundAssetIds.ToList())
        {
            var asset = findMediaAsset(pair.Value);
            if (asset is null || !isVisualMediaAsset(asset))
            {
                sceneBackgroundAssetIds.Remove(pair.Key);
            }
        }
    }
}
