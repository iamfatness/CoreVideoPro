using System.Security.Cryptography;
using System.Text;
using System.Buffers.Binary;

namespace CoreVideoPro.MediaCore.Services;

public sealed record MediaBinAssetDescriptor(
    string Id,
    string Name,
    string Kind,
    int? DurationMs,
    string RelativePath,
    string FilePath,
    string FileType);

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

    public static string BuildFileType(string fileName)
    {
        var extension = Path.GetExtension(fileName).TrimStart('.');
        return string.IsNullOrWhiteSpace(extension) ? "file" : extension.ToUpperInvariant();
    }

    public static bool IsUsableMediaFile(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath) || !File.Exists(filePath))
        {
            return false;
        }

        try
        {
            var extension = Path.GetExtension(filePath).ToLowerInvariant();
            return extension switch
            {
                ".png" => IsCompletePng(filePath),
                ".jpg" or ".jpeg" => IsCompleteJpeg(filePath),
                ".gif" => IsCompleteGif(filePath),
                _ => new FileInfo(filePath).Length > 0
            };
        }
        catch
        {
            return false;
        }
    }

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

                    // Lifecycle L4: skip dot-folders (e.g. the .trash recycle bin
                    // where removed assets are moved) so removed media never
                    // re-appears in the bin.
                    if (relativePath.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                        .Any(segment => segment.StartsWith('.')))
                    {
                        continue;
                    }

                    var kind = ClassifyFile(relativePath, fileName);
                    if (kind is null || !IsUsableMediaFile(filePath))
                    {
                        continue;
                    }

                    assets.Add(new MediaBinAssetDescriptor(
                        BuildAssetId(relativePath),
                        BuildAssetName(fileName),
                        kind,
                        DurationMs: null,
                        RelativePath: relativePath,
                        FilePath: filePath,
                        FileType: BuildFileType(fileName)));
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

    private static bool IsCompletePng(string filePath)
    {
        using var stream = File.OpenRead(filePath);
        Span<byte> signature = stackalloc byte[8];
        ReadOnlySpan<byte> expectedSignature =
            [0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A];
        if (stream.Read(signature) != signature.Length ||
            !signature.SequenceEqual(expectedSignature))
        {
            return false;
        }

        var foundHeader = false;
        var foundImageData = false;
        Span<byte> chunkHeader = stackalloc byte[8];
        Span<byte> dimensions = stackalloc byte[8];
        while (stream.Position + 12 <= stream.Length)
        {
            if (stream.Read(chunkHeader) != chunkHeader.Length)
            {
                return false;
            }

            var length = BinaryPrimitives.ReadUInt32BigEndian(chunkHeader[..4]);
            var chunkType = Encoding.ASCII.GetString(chunkHeader[4..8]);
            if (length > int.MaxValue || stream.Position + length + 4 > stream.Length)
            {
                return false;
            }

            if (chunkType == "IHDR")
            {
                if (foundHeader || length != 13)
                {
                    return false;
                }

                if (stream.Read(dimensions) != dimensions.Length ||
                    BinaryPrimitives.ReadUInt32BigEndian(dimensions[..4]) == 0 ||
                    BinaryPrimitives.ReadUInt32BigEndian(dimensions[4..]) == 0)
                {
                    return false;
                }
                stream.Seek(length - dimensions.Length, SeekOrigin.Current);
                foundHeader = true;
            }
            else
            {
                stream.Seek(length, SeekOrigin.Current);
            }

            stream.Seek(4, SeekOrigin.Current); // CRC
            foundImageData |= chunkType == "IDAT" && length > 0;
            if (chunkType == "IEND")
            {
                return foundHeader && foundImageData && length == 0;
            }
        }

        return false;
    }

    private static bool IsCompleteJpeg(string filePath)
    {
        using var stream = File.OpenRead(filePath);
        if (stream.Length < 4 || stream.ReadByte() != 0xFF || stream.ReadByte() != 0xD8)
        {
            return false;
        }
        stream.Seek(-2, SeekOrigin.End);
        return stream.ReadByte() == 0xFF && stream.ReadByte() == 0xD9;
    }

    private static bool IsCompleteGif(string filePath)
    {
        using var stream = File.OpenRead(filePath);
        Span<byte> header = stackalloc byte[6];
        if (stream.Length < 14 || stream.Read(header) != header.Length)
        {
            return false;
        }
        var validHeader = header.SequenceEqual("GIF87a"u8) || header.SequenceEqual("GIF89a"u8);
        stream.Seek(-1, SeekOrigin.End);
        return validHeader && stream.ReadByte() == 0x3B;
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
