using CoreVideoPro.WinUI.Services;
using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MediaBinServiceTests
{
    [Fact]
    public void ImportFiles_ProbesImageDimensionsForSceneFraming()
    {
        var tempFolder = Path.Combine(Path.GetTempPath(), $"corevideo-media-{Guid.NewGuid():N}");
        var mediaRoot = Path.Combine(tempFolder, "library");
        Directory.CreateDirectory(tempFolder);
        var sourcePath = Path.Combine(tempFolder, "wide-slate.png");
        File.WriteAllBytes(sourcePath, CreatePng(width: 1600, height: 900));

        try
        {
            var asset = Assert.Single(new MediaBinService(mediaRoot).ImportFiles([sourcePath]));

            Assert.Equal(1600, asset.NaturalWidth);
            Assert.Equal(900, asset.NaturalHeight);
            Assert.Contains("1600x900", asset.DetailLabel, StringComparison.Ordinal);
            Assert.StartsWith(mediaRoot, asset.FilePath, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            Directory.Delete(tempFolder, recursive: true);
        }
    }

    [Fact]
    public void ImportFiles_RejectsTruncatedImage()
    {
        var tempFolder = Path.Combine(Path.GetTempPath(), $"corevideo-media-{Guid.NewGuid():N}");
        var mediaRoot = Path.Combine(tempFolder, "library");
        Directory.CreateDirectory(tempFolder);
        var sourcePath = Path.Combine(tempFolder, "broken.png");
        File.WriteAllBytes(sourcePath, [0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A]);

        try
        {
            Assert.Empty(new MediaBinService(mediaRoot).ImportFiles([sourcePath]));
            Assert.Empty(Directory.EnumerateFileSystemEntries(mediaRoot));
        }
        finally
        {
            Directory.Delete(tempFolder, recursive: true);
        }
    }

    private static byte[] CreatePng(int width, int height)
    {
        using var png = new MemoryStream();
        png.Write([0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A]);

        var header = new byte[13];
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(0, 4), width);
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(4, 4), height);
        header[8] = 8; // grayscale, 8-bit
        WriteChunk(png, "IHDR", header);

        var scanlines = new byte[(width + 1) * height];
        using var compressed = new MemoryStream();
        using (var zlib = new ZLibStream(compressed, CompressionLevel.SmallestSize, leaveOpen: true))
        {
            zlib.Write(scanlines);
        }
        WriteChunk(png, "IDAT", compressed.ToArray());
        WriteChunk(png, "IEND", []);
        return png.ToArray();
    }

    private static void WriteChunk(Stream stream, string type, byte[] data)
    {
        Span<byte> length = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(length, data.Length);
        stream.Write(length);
        var typeBytes = Encoding.ASCII.GetBytes(type);
        stream.Write(typeBytes);
        stream.Write(data);

        Span<byte> crc = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(crc, ComputeCrc32(typeBytes, data));
        stream.Write(crc);
    }

    private static uint ComputeCrc32(byte[] type, byte[] data)
    {
        var crc = uint.MaxValue;
        foreach (var value in type.Concat(data))
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
            {
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
            }
        }
        return ~crc;
    }
}
