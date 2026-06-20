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

    public static bool HasPresentedFrame(Image image) =>
        PreviewStates.TryGetValue(image, out var state) &&
        Volatile.Read(ref state.HasPresentedFrame) == 1;

    public static void ClearPreview(Image image)
    {
        if (PreviewStates.TryGetValue(image, out var state))
        {
            Interlocked.Exchange(ref state.HasPresentedFrame, 0);
            Interlocked.Exchange(ref state.UpdateInFlight, 0);
        }

        image.Source = null;
        image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
    }

    public static void SetPreview(Image image, byte[]? bgra, int width, int height)
    {
        var state = PreviewStates.GetOrCreateValue(image);
        if (bgra is not { Length: > 0 } || width <= 0 || height <= 0 || bgra.Length != width * height * 4)
        {
            if (Volatile.Read(ref state.HasPresentedFrame) == 0)
            {
                image.Source = null;
                image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            }

            return;
        }

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
            _ = SetPreviewSourceAsync(image, state, softwareBitmap, width, height);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"preview: failed to update BGRA preview {width}x{height}: {ex.GetType().Name}: {ex.Message}");
            Interlocked.Exchange(ref state.UpdateInFlight, 0);
            if (Volatile.Read(ref state.HasPresentedFrame) == 0)
            {
                image.Source = null;
                image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            }
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
            var nextSource = state.NextSource;
            await nextSource.SetBitmapAsync(bitmap);
            image.Source = nextSource;
            image.Visibility = Microsoft.UI.Xaml.Visibility.Visible;
            state.CommitPresentedSource();
            Interlocked.Exchange(ref state.HasPresentedFrame, 1);
        }
        catch (Exception ex)
        {
            if (Interlocked.Increment(ref state.FailureCount) <= 3)
            {
                LaunchLog.Write($"preview: failed to update BGRA preview {width}x{height}: {ex.GetType().Name}: {ex.Message}");
            }

            if (Volatile.Read(ref state.HasPresentedFrame) == 0)
            {
                image.Source = null;
                image.Visibility = Microsoft.UI.Xaml.Visibility.Collapsed;
            }
        }
        finally
        {
            bitmap.Dispose();
            Interlocked.Exchange(ref state.UpdateInFlight, 0);
        }
    }

    private sealed class PreviewState
    {
        private readonly SoftwareBitmapSource[] _sources = [new(), new()];
        private int _frontSourceIndex;

        public int UpdateInFlight;
        public int FailureCount;
        public int HasPresentedFrame;
        public long LastUpdateMs;

        public SoftwareBitmapSource NextSource => _sources[1 - Volatile.Read(ref _frontSourceIndex)];

        public void CommitPresentedSource() =>
            Interlocked.Exchange(ref _frontSourceIndex, 1 - Volatile.Read(ref _frontSourceIndex));
    }
}
