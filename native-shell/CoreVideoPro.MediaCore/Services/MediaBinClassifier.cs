using System.Security.Cryptography;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

public sealed record MediaBinAssetDescriptor(
    string Id,
    string Name,
    string Kind,
    int? DurationMs);

public sealed record MediaBinGroupDescriptor(
    string Kind,
    string Label,
    IReadOnlyList<MediaBinAssetDescriptor> Assets);

public static class MediaBinClassifier
{
    public static readonly string[] MediaAssetKinds = ["stinger", "lower-third", "audio-bed", "slate"];

    private static readonly HashSet<string> VideoExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".mp4", ".mov", ".webm"
    };

    private static readonly HashSet<string> ImageExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".gif"
    };

    private static readonly HashSet<string> AudioExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".wav", ".mp3", ".aac", ".m4a"
    };

    public static string DefaultMediaRoot =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "media");

    public static string? ResolvePackagedMediaRoot()
    {
        var packaged = Path.Combine(AppContext.BaseDirectory, "media");
        return Directory.Exists(packaged) ? packaged : null;
    }

    public static IReadOnlyList<string> ResolveMediaRoots()
    {
        var roots = new List<string>();

        try
        {
            var localRoot = DefaultMediaRoot;
            Directory.CreateDirectory(localRoot);
            roots.Add(localRoot);
        }
        catch
        {
            // Best effort — packaged media may still be available.
        }

        var packagedRoot = ResolvePackagedMediaRoot();
        if (!string.IsNullOrWhiteSpace(packagedRoot))
        {
            roots.Add(packagedRoot);
        }

        return roots;
    }

    public static string? ClassifyFile(string relativePath, string fileName)
    {
        if (IsSlateAsset(relativePath, fileName))
        {
            return "slate";
        }

        var extension = Path.GetExtension(fileName);
        if (VideoExtensions.Contains(extension))
        {
            return "stinger";
        }

        if (ImageExtensions.Contains(extension))
        {
            return "lower-third";
        }

        if (AudioExtensions.Contains(extension))
        {
            return "audio-bed";
        }

        return null;
    }

    public static bool IsSlateAsset(string relativePath, string fileName)
    {
        var normalizedRelative = relativePath.Replace('\\', '/');
        if (normalizedRelative.Contains("slates/", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        var baseName = Path.GetFileNameWithoutExtension(fileName);
        return baseName.StartsWith("slate", StringComparison.OrdinalIgnoreCase);
    }

    public static string BuildAssetId(string relativePath)
    {
        var normalized = relativePath.Replace('\\', '/');
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(normalized));
        return $"media-{Convert.ToHexString(hash)[..12].ToLowerInvariant()}";
    }

    public static string BuildAssetName(string fileName) =>
        Path.GetFileNameWithoutExtension(fileName);

    public static string KindLabel(string kind) => kind switch
    {
        "stinger" => "Stinger",
        "lower-third" => "Lower-third",
        "audio-bed" => "Audio bed",
        "slate" => "Slate",
        _ => kind
    };

    public static IReadOnlyList<MediaBinAssetDescriptor> ScanRoot(string root)
    {
        if (!Directory.Exists(root))
        {
            return [];
        }

        var assets = new List<MediaBinAssetDescriptor>();

        try
        {
            foreach (var filePath in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
            {
                try
                {
                    var relativePath = Path.GetRelativePath(root, filePath);
                    var fileName = Path.GetFileName(filePath);
                    if (string.IsNullOrWhiteSpace(fileName))
                    {
                        continue;
                    }

                    var kind = ClassifyFile(relativePath, fileName);
                    if (kind is null)
                    {
                        continue;
                    }

                    assets.Add(new MediaBinAssetDescriptor(
                        BuildAssetId(relativePath),
                        BuildAssetName(fileName),
                        kind,
                        DurationMs: null));
                }
                catch
                {
                    // Skip unreadable files.
                }
            }
        }
        catch
        {
            return assets;
        }

        return assets;
    }

    public static IReadOnlyList<MediaBinGroupDescriptor> GroupAssets(IReadOnlyList<MediaBinAssetDescriptor> assets) =>
        MediaAssetKinds
            .Select(kind => new MediaBinGroupDescriptor(
                kind,
                KindLabel(kind),
                assets.Where(asset => asset.Kind == kind).ToList()))
            .Where(group => group.Assets.Count > 0)
            .ToList();

    public static IReadOnlyList<MediaBinAssetDescriptor> ScanMediaRoots(IEnumerable<string> roots)
    {
        var assets = new List<MediaBinAssetDescriptor>();
        var seenIds = new HashSet<string>(StringComparer.Ordinal);

        foreach (var root in roots)
        {
            foreach (var asset in ScanRoot(root))
            {
                if (seenIds.Add(asset.Id))
                {
                    assets.Add(asset);
                }
            }
        }

        return assets;
    }

    public static string BuildEmptyGuidanceMessage(IEnumerable<string>? roots = null)
    {
        var resolvedRoots = (roots?.Where(root => !string.IsNullOrWhiteSpace(root))
            ?? ResolveMediaRoots())
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        if (resolvedRoots.Count == 0)
        {
            resolvedRoots.Add(DefaultMediaRoot);
        }

        var lines = new List<string>
        {
            "Add stingers (.mp4), lower-thirds (.png), audio beds (.wav/.mp3), or slates (slates/ or slate-*) to:"
        };
        lines.AddRange(resolvedRoots);

        return string.Join(Environment.NewLine, lines);
    }
}