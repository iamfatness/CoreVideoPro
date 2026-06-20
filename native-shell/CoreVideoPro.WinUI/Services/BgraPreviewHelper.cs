using System.Runtime.CompilerServices;
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
    private const long MinimumPreviewIntervalMs = 16;
    private static readonly ConditionalWeakTable<Image, PreviewState> PreviewStates = new();

    public static void SetPreview(Image image, byte[]? bgra, int width, int height)
    {
        if (bgra is not { Length: > 0 } || width <= 0 || height <= 0 || bgra.Length != width * height * 4)
        {
            image.Source = null;
            image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            return;
        }

        var state = PreviewStates.GetOrCreateValue(image);
        var now = Environment.TickCount64;
        if (now - Interlocked.Read(ref state.LastUpdateMs) < MinimumPreviewIntervalMs ||
            Interlocked.CompareExchange(ref state.UpdateInFlight, 1, 0) != 0)
        {
            return;
        }

        Interlocked.Exchange(ref state.LastUpdateMs, now);
        try
        {
            var softwareBitmap = new SoftwareBitmap(BitmapPixelFormat.Bgra8, width, height, BitmapAlphaMode.Ignore);
            softwareBitmap.CopyFromBuffer(bgra.AsBuffer());
            if (!ReferenceEquals(image.Source, state.Source))
            {
                image.Source = state.Source;
            }

            _ = SetPreviewSourceAsync(image, state, softwareBitmap, width, height);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"preview: failed to update BGRA preview {width}x{height}: {ex.GetType().Name}: {ex.Message}");
            Interlocked.Exchange(ref state.UpdateInFlight, 0);
            image.Source = null;
            image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        }
    }

    private static async Task SetPreviewSourceAsync(
        Image image,
        PreviewState state,
        SoftwareBitmap bitmap,
        int width,
        int height)
    {
        try
        {
            await state.Source.SetBitmapAsync(bitmap);
            image.Source = state.Source;
            image.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
        }
        catch (Exception ex)
        {
            if (Interlocked.Increment(ref state.FailureCount) <= 3)
            {
                LaunchLog.Write($"preview: failed to update BGRA preview {width}x{height}: {ex.GetType().Name}: {ex.Message}");
            }

            image.Source = null;
            image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
        }
        finally
        {
            bitmap.Dispose();
            Interlocked.Exchange(ref state.UpdateInFlight, 0);
        }
    }

    private sealed class PreviewState
    {
        public SoftwareBitmapSource Source { get; } = new();
        public int UpdateInFlight;
        public int FailureCount;
        public long LastUpdateMs;
    }
}
