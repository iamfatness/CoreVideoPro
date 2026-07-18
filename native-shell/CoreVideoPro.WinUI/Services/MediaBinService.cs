using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using System.Buffers.Binary;

namespace CoreVideoPro.WinUI.Services;

public sealed class MediaBinService
{
    private readonly string _mediaRoot;
    private readonly IReadOnlyList<string> _mediaRoots;

    public MediaBinService(string? mediaRoot = null, IReadOnlyList<string>? mediaRoots = null)
    {
        _mediaRoot = string.IsNullOrWhiteSpace(mediaRoot)
            ? MediaBinClassifier.DefaultMediaRoot
            : Path.GetFullPath(mediaRoot);
        _mediaRoots = mediaRoots ?? (string.IsNullOrWhiteSpace(mediaRoot)
            ? MediaBinClassifier.ResolveMediaRoots()
            : [_mediaRoot]);
    }

    public IReadOnlyList<string> SupportedExtensions { get; } =
        [".mp4", ".mov", ".webm", ".png", ".jpg", ".jpeg", ".gif", ".wav", ".mp3", ".aac", ".m4a"];

    public IReadOnlyList<MediaBinGroup> LoadGroups()
    {
        try
        {
            var assets = MediaBinClassifier.ScanMediaRoots(_mediaRoots);
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
        Directory.CreateDirectory(_mediaRoot);

        foreach (var sourcePath in filePaths.Where(File.Exists))
        {
            var fileName = Path.GetFileName(sourcePath);
            if (string.IsNullOrWhiteSpace(fileName))
            {
                continue;
            }

            var kind = MediaBinClassifier.ClassifyFile(fileName, fileName);
            if (kind is null || !MediaBinClassifier.IsUsableMediaFile(sourcePath))
            {
                continue;
            }

            var folder = Path.Combine(_mediaRoot, kind);
            Directory.CreateDirectory(folder);
            var destinationPath = BuildUniqueDestinationPath(folder, fileName);
            File.Copy(sourcePath, destinationPath);

            var relativePath = Path.GetRelativePath(_mediaRoot, destinationPath);
            var dimensions = ProbeMediaDimensions(destinationPath, kind);
            imported.Add(new MediaAsset
            {
                Id = MediaBinClassifier.BuildAssetId(relativePath),
                Name = MediaBinClassifier.BuildAssetName(fileName),
                Kind = kind,
                RelativePath = relativePath,
                FilePath = destinationPath,
                FileType = MediaBinClassifier.BuildFileType(fileName),
                NaturalWidth = dimensions.Width,
                NaturalHeight = dimensions.Height
            });
        }

        return imported;
    }

    // Lifecycle L4 (source-lifecycle-spec): remove an asset from the bin.
    // NON-DESTRUCTIVE by design (professional product): the file is moved to a
    // .trash subfolder of the media root, not hard-deleted, so an accidental
    // remove is recoverable from disk. Returns true when the file was moved.
    public bool DeleteAsset(MediaAsset asset)
    {
        if (asset is null || string.IsNullOrWhiteSpace(asset.FilePath) || !File.Exists(asset.FilePath))
        {
            return false;
        }

        try
        {
            var trashFolder = Path.Combine(_mediaRoot, ".trash");
            Directory.CreateDirectory(trashFolder);
            var destination = BuildUniqueDestinationPath(trashFolder, Path.GetFileName(asset.FilePath));
            File.Move(asset.FilePath, destination);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static MediaBinGroup ToMediaBinGroup(MediaBinGroupDescriptor group) =>
        new()
        {
            Kind = group.Kind,
            Label = group.Label,
            Assets = group.Assets.Select(ToMediaAsset).ToList()
        };

    private static MediaAsset ToMediaAsset(MediaBinAssetDescriptor asset)
    {
        var dimensions = ProbeMediaDimensions(asset.FilePath, asset.Kind);
        return new MediaAsset
        {
            Id = asset.Id,
            Name = asset.Name,
            Kind = asset.Kind,
            DurationMs = asset.DurationMs,
            RelativePath = asset.RelativePath,
            FilePath = asset.FilePath,
            FileType = asset.FileType,
            NaturalWidth = dimensions.Width,
            NaturalHeight = dimensions.Height
        };
    }

    private static (int? Width, int? Height) ProbeMediaDimensions(string filePath, string kind)
    {
        var extension = Path.GetExtension(filePath).ToLowerInvariant();
        try
        {
            if (extension == ".png")
            {
                return ProbePngDimensions(filePath);
            }

            if (extension is ".jpg" or ".jpeg")
            {
                return ProbeJpegDimensions(filePath);
            }

            if (extension == ".gif")
            {
                return ProbeGifDimensions(filePath);
            }
        }
        catch
        {
            // Dimension probing is best-effort; media remains usable without metadata.
        }

        // Do not invent 1080p metadata for videos whose container has not been
        // probed. The local player and native decoder still use their real size.
        return (null, null);
    }

    private static (int? Width, int? Height) ProbePngDimensions(string filePath)
    {
        Span<byte> header = stackalloc byte[24];
        using var stream = File.OpenRead(filePath);
        if (stream.Read(header) < header.Length ||
            header[0] != 0x89 ||
            header[1] != (byte)'P' ||
            header[2] != (byte)'N' ||
            header[3] != (byte)'G')
        {
            return (null, null);
        }

        return (
            BinaryPrimitives.ReadInt32BigEndian(header[16..20]),
            BinaryPrimitives.ReadInt32BigEndian(header[20..24]));
    }

    private static (int? Width, int? Height) ProbeGifDimensions(string filePath)
    {
        Span<byte> header = stackalloc byte[10];
        using var stream = File.OpenRead(filePath);
        if (stream.Read(header) < header.Length ||
            header[0] != (byte)'G' ||
            header[1] != (byte)'I' ||
            header[2] != (byte)'F')
        {
            return (null, null);
        }

        return (
            BinaryPrimitives.ReadUInt16LittleEndian(header[6..8]),
            BinaryPrimitives.ReadUInt16LittleEndian(header[8..10]));
    }

    private static (int? Width, int? Height) ProbeJpegDimensions(string filePath)
    {
        using var stream = File.OpenRead(filePath);
        if (stream.ReadByte() != 0xFF || stream.ReadByte() != 0xD8)
        {
            return (null, null);
        }

        while (stream.Position < stream.Length)
        {
            var markerPrefix = stream.ReadByte();
            if (markerPrefix != 0xFF)
            {
                continue;
            }

            int marker;
            do
            {
                marker = stream.ReadByte();
            }
            while (marker == 0xFF);

            if (marker < 0)
            {
                break;
            }

            if (marker is 0xD8 or 0xD9)
            {
                continue;
            }

            var length = ReadBigEndianUInt16(stream);
            if (length < 2)
            {
                break;
            }

            if (marker is 0xC0 or 0xC1 or 0xC2 or 0xC3 or 0xC5 or 0xC6 or 0xC7 or 0xC9 or 0xCA or 0xCB or 0xCD or 0xCE or 0xCF)
            {
                _ = stream.ReadByte();
                var height = ReadBigEndianUInt16(stream);
                var width = ReadBigEndianUInt16(stream);
                return (width, height);
            }

            stream.Seek(length - 2, SeekOrigin.Current);
        }

        return (null, null);
    }

    private static int ReadBigEndianUInt16(Stream stream)
    {
        var high = stream.ReadByte();
        var low = stream.ReadByte();
        return high < 0 || low < 0 ? 0 : (high << 8) | low;
    }

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
