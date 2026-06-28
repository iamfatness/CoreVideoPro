using System.Runtime.CompilerServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Renders BGRA frame bytes into a WinUI <see cref="Image"/> via a REUSED
/// <see cref="WriteableBitmap"/>. The previous implementation allocated a new
/// SoftwareBitmap per frame and pushed it through SoftwareBitmapSource.SetBitmapAsync
/// — an async GPU round-trip that, at 1080p (capture cards), could not keep up and
/// cancelled (TaskCanceledException), freezing every monitor. A WriteableBitmap is
/// allocated once per size and updated with a synchronous pixel-buffer copy +
/// Invalidate, which is what WinUI is designed to use for CPU video.
///
/// SetPreview must be called on the UI thread (it touches WriteableBitmap and
/// Image.Source) — every caller is a binding/UI callback, which is already the case.
/// </summary>
public static class BgraPreviewHelper
{
    private const long MinimumPreviewIntervalMs = 16;
    private static readonly ConditionalWeakTable<Image, PreviewState> PreviewStates = new();

    public static bool HasPresentedFrame(Image image) =>
        PreviewStates.TryGetValue(image, out var state) && state.HasPresented;

    public static void ClearPreview(Image image)
    {
        if (PreviewStates.TryGetValue(image, out var state))
        {
            state.HasPresented = false;
        }

        image.Source = null;
        image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
    }

    public static void SetPreview(Image image, byte[]? bgra, int width, int height)
    {
        var state = PreviewStates.GetOrCreateValue(image);
        if (bgra is not { Length: > 0 } || width <= 0 || height <= 0 || bgra.Length != width * height * 4)
        {
            if (!state.HasPresented)
            {
                image.Source = null;
                image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            }

            return;
        }

        var now = Environment.TickCount64;
        if (now - state.LastUpdateMs < MinimumPreviewIntervalMs)
        {
            return;
        }

        state.LastUpdateMs = now;
        try
        {
            var bitmap = state.Bitmap;
            if (bitmap is null || bitmap.PixelWidth != width || bitmap.PixelHeight != height)
            {
                bitmap = new WriteableBitmap(width, height);
                state.Bitmap = bitmap;
                image.Source = bitmap;
            }

            // Synchronous BGRA copy into the reusable back buffer + invalidate. No
            // per-frame allocation, no async source swap.
            using (var stream = bitmap.PixelBuffer.AsStream())
            {
                stream.Write(bgra, 0, bgra.Length);
            }

            bitmap.Invalidate();
            image.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
            state.HasPresented = true;
        }
        catch (Exception ex)
        {
            if (state.FailureCount++ < 3)
            {
                LaunchLog.Write($"preview: WriteableBitmap update failed {width}x{height}: {ex.GetType().Name}: {ex.Message}");
            }

            if (!state.HasPresented)
            {
                image.Source = null;
                image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            }
        }
    }

    private sealed class PreviewState
    {
        public WriteableBitmap? Bitmap;
        public long LastUpdateMs;
        public int FailureCount;
        public bool HasPresented;
    }
}
