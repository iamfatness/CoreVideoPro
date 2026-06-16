using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Decodes BGRA thumbnail bytes into a WinUI <see cref="Image"/> source.
/// </summary>
public static class BgraPreviewHelper
{
    public static void SetPreview(Image image, byte[]? bgra, int width, int height)
    {
        if (bgra is not { Length: > 0 } || width <= 0 || height <= 0 || bgra.Length != width * height * 4)
        {
            image.Source = null;
            image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            return;
        }

        var softwareBitmap = new SoftwareBitmap(BitmapPixelFormat.Bgra8, width, height, BitmapAlphaMode.Ignore);
        softwareBitmap.CopyFromBuffer(bgra.AsBuffer());
        var source = new SoftwareBitmapSource();
        _ = SetPreviewSourceAsync(image, source, softwareBitmap);
    }

    private static async Task SetPreviewSourceAsync(Image image, SoftwareBitmapSource source, SoftwareBitmap bitmap)
    {
        await source.SetBitmapAsync(bitmap);
        image.Source = source;
        image.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
    }
}