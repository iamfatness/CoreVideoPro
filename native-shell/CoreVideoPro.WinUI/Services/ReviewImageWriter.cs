using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media.Imaging;
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
        // Let Fluent expansion/theme transitions settle before sampling the
        // offscreen tree. Immediate captures show transient white control plates.
        root.UpdateLayout();await Task.Delay(750);root.UpdateLayout();
        var bitmap = new RenderTargetBitmap();
        await bitmap.RenderAsync(root);
        var pixels = await bitmap.GetPixelsAsync();
        using var reader = DataReader.FromBuffer(pixels);
        var bytes = new byte[pixels.Length];
        reader.ReadBytes(bytes);
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var directory = await StorageFolder.GetFolderFromPathAsync(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var file = await directory.CreateFileAsync(Path.GetFileName(path), CreationCollisionOption.ReplaceExisting);
        using var stream = await file.OpenAsync(FileAccessMode.ReadWrite);
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
            (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, bytes);
        await encoder.FlushAsync();
    }
}
