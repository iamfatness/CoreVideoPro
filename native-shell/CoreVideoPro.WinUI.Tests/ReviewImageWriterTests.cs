using CoreVideoPro.WinUI.Services;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ReviewImageWriterTests
{
    [Fact]
    public async Task PngCompositesTranslucentControlPixelsOverTheDeclaredBackdrop()
    {
        var directory = Path.Combine(Path.GetTempPath(), "CoreVideoPro-review-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "controls.png");
        try
        {
            // BGRA from a transparent XAML subtree: a low-opacity white Fluent
            // fill, an antialiased edge, a hole and an already opaque panel.
            byte[] pixels = [15, 15, 15, 15, 64, 32, 16, 128, 0, 0, 0, 0, 24, 17, 12, 255];
            await ReviewImageWriter.SavePixelsAsync(pixels, 2, 2,
                Windows.UI.Color.FromArgb(255, 10, 11, 12), path);
            var file = await StorageFile.GetFileFromPathAsync(path);
            using var stream = await file.OpenAsync(FileAccessMode.Read);
            var decoder = await BitmapDecoder.CreateAsync(stream);
            Assert.Equal(2u, decoder.PixelWidth);
            Assert.Equal(2u, decoder.PixelHeight);
            var data = await decoder.GetPixelDataAsync(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Straight,
                new BitmapTransform(), ExifOrientationMode.IgnoreExifOrientation, ColorManagementMode.DoNotColorManage);
            Assert.Equal(new byte[] { 26, 25, 24, 255, 70, 37, 21, 255, 12, 11, 10, 255, 24, 17, 12, 255 }, data.DetachPixelData());
            Assert.Equal(new byte[] { 15, 15, 15, 15, 64, 32, 16, 128, 0, 0, 0, 0, 24, 17, 12, 255 }, pixels);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
            Directory.Delete(directory);
        }
    }
}
