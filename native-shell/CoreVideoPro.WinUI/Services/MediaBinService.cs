using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public sealed class MediaBinService
{
    public IReadOnlyList<string> SupportedExtensions { get; } =
        [".mp4", ".mov", ".webm", ".png", ".jpg", ".jpeg", ".gif", ".wav", ".mp3", ".aac", ".m4a"];

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

    public IReadOnlyList<MediaAsset> ImportFiles(IEnumerable<string> filePaths)
    {
        var imported = new List<MediaAsset>();
        Directory.CreateDirectory(MediaBinClassifier.DefaultMediaRoot);

        foreach (var sourcePath in filePaths.Where(File.Exists))
        {
            var fileName = Path.GetFileName(sourcePath);
            if (string.IsNullOrWhiteSpace(fileName))
            {
                continue;
            }

            var kind = MediaBinClassifier.ClassifyFile(fileName, fileName);
            if (kind is null)
            {
                continue;
            }

            var folder = Path.Combine(MediaBinClassifier.DefaultMediaRoot, kind);
            Directory.CreateDirectory(folder);
            var destinationPath = BuildUniqueDestinationPath(folder, fileName);
            File.Copy(sourcePath, destinationPath);

            var relativePath = Path.GetRelativePath(MediaBinClassifier.DefaultMediaRoot, destinationPath);
            imported.Add(new MediaAsset
            {
                Id = MediaBinClassifier.BuildAssetId(relativePath),
                Name = MediaBinClassifier.BuildAssetName(fileName),
                Kind = kind,
                RelativePath = relativePath,
                FilePath = destinationPath,
                FileType = MediaBinClassifier.BuildFileType(fileName)
            });
        }

        return imported;
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
            DurationMs = asset.DurationMs,
            RelativePath = asset.RelativePath,
            FilePath = asset.FilePath,
            FileType = asset.FileType
        };

    private static string BuildUniqueDestinationPath(string folder, string fileName)
    {
        var destinationPath = Path.Combine(folder, fileName);
        if (!File.Exists(destinationPath))
        {
            return destinationPath;
        }

        var name = Path.GetFileNameWithoutExtension(fileName);
        var extension = Path.GetExtension(fileName);
        for (var index = 2; ; index++)
        {
            destinationPath = Path.Combine(folder, $"{name}-{index}{extension}");
            if (!File.Exists(destinationPath))
            {
                return destinationPath;
            }
        }
    }
}
