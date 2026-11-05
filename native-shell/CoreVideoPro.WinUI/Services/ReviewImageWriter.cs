using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using CoreVideoPro.WinUI.Controls;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Streams;

namespace CoreVideoPro.WinUI.Services;

// Real compiled controls only. SwapChainPanel video is deliberately not claimed
// by RenderTargetBitmap; native pixel probes qualify that separate surface.
internal static class ReviewImageWriter
{
    internal static async Task SaveAsync(FrameworkElement root, string path)
    {
        // Let Fluent expansion/theme transitions settle before sampling. The
        // encoding step separately supplies the shell behind transparent roots.
        root.UpdateLayout();await Task.Delay(750);root.UpdateLayout();
        var bitmap = new RenderTargetBitmap();
        await bitmap.RenderAsync(root);
        var pixels = await bitmap.GetPixelsAsync();
        using var reader = DataReader.FromBuffer(pixels);
        var bytes = new byte[pixels.Length];
        reader.ReadBytes(bytes);
        if (StudioBrushes.Background is not SolidColorBrush backdrop)
            throw new InvalidOperationException("Review captures require a declared solid Studio backdrop.");
        await SavePixelsAsync(bytes, (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, backdrop.Color, path);
    }

    internal static async Task SavePixelsAsync(byte[] pixels, uint width, uint height, Windows.UI.Color backdrop, string path)
    {
        if (backdrop.A != 255) throw new ArgumentException("Review captures require an opaque backdrop.", nameof(backdrop));
        if (width == 0 || height == 0 || (ulong)width * height * 4 != (ulong)pixels.LongLength)
            throw new ArgumentException("Review capture dimensions do not match its pixels.", nameof(pixels));
        // RenderTargetBitmap contains premultiplied BGRA. A transparent subtree
        // omits the shell behind it; the resulting straight-alpha PNG depends
        // on its viewer's backdrop and alpha support for Fluent fills/text edges.
        // Source-over the actual Studio backdrop before encoding so each review
        // artifact is opaque and does not depend on its viewer's alpha handling.
        var opaque = new byte[pixels.Length];
        for (var offset = 0; offset < pixels.Length; offset += 4)
        {
            var remaining = 255 - pixels[offset + 3];
            opaque[offset] = Composite(pixels[offset], backdrop.B, remaining);
            opaque[offset + 1] = Composite(pixels[offset + 1], backdrop.G, remaining);
            opaque[offset + 2] = Composite(pixels[offset + 2], backdrop.R, remaining);
            opaque[offset + 3] = 255;
        }
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var directory = await StorageFolder.GetFolderFromPathAsync(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var file = await directory.CreateFileAsync(Path.GetFileName(path), CreationCollisionOption.ReplaceExisting);
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
            width, height, 96, 96, opaque);
        await encoder.FlushAsync();
    }

    private static byte Composite(byte foreground, byte background, int remaining) =>
        (byte)Math.Min(255, foreground + (background * remaining + 127) / 255);
}
