using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MediaBinServiceTests
{
    [Fact]
    public void ImportFiles_ProbesImageDimensionsForSceneFraming()
    {
        var tempFolder = Path.Combine(Path.GetTempPath(), $"corevideo-media-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempFolder);
        var sourcePath = Path.Combine(tempFolder, "wide-slate.png");
        File.WriteAllBytes(sourcePath, MinimalPngHeader(width: 1600, height: 900));

        try
        {
            var asset = Assert.Single(new MediaBinService().ImportFiles([sourcePath]));

            Assert.Equal(1600, asset.NaturalWidth);
            Assert.Equal(900, asset.NaturalHeight);
            Assert.Contains("1600x900", asset.DetailLabel, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(tempFolder, recursive: true);
        }
    }

    private static byte[] MinimalPngHeader(int width, int height)
    {
        var bytes = new byte[24];
        bytes[0] = 0x89;
        bytes[1] = (byte)'P';
        bytes[2] = (byte)'N';
        bytes[3] = (byte)'G';
        bytes[4] = 0x0D;
        bytes[5] = 0x0A;
        bytes[6] = 0x1A;
        bytes[7] = 0x0A;
        WriteBigEndian(bytes, 16, width);
        WriteBigEndian(bytes, 20, height);
        return bytes;
    }

    private static void WriteBigEndian(byte[] bytes, int offset, int value)
    {
        bytes[offset] = (byte)((value >> 24) & 0xFF);
        bytes[offset + 1] = (byte)((value >> 16) & 0xFF);
        bytes[offset + 2] = (byte)((value >> 8) & 0xFF);
        bytes[offset + 3] = (byte)(value & 0xFF);
    }
}
