using CoreVideoPro.WinUI.Models;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.Capture.Frames;
using Windows.Media.MediaProperties;

namespace CoreVideoPro.WinUI.Services;

public sealed class CaptureDeviceFrameReaderService : IDisposable
{
    private readonly Dictionary<string, CaptureSession> _sessions = new(StringComparer.Ordinal);

    public async Task StartAsync(CaptureDevice device)
    {
        if (_sessions.ContainsKey(device.Id))
        {
            return;
        }

        var session = new CaptureSession(device.Id, device.NativeDeviceId);
        try
        {
            await session.StartAsync().ConfigureAwait(false);
            _sessions[device.Id] = session;
        }
        catch
        {
            await session.DisposeAsync().ConfigureAwait(false);
            throw;
        }
    }

    public async Task StopAsync(string deviceId)
    {
        if (!_sessions.Remove(deviceId, out var session))
        {
            return;
        }

        await session.DisposeAsync().ConfigureAwait(false);
    }

    public void Dispose()
    {
        foreach (var session in _sessions.Values.ToList())
        {
            session.DisposeAsync().AsTask().GetAwaiter().GetResult();
        }

        _sessions.Clear();
    }

    private sealed class CaptureSession : IAsyncDisposable
    {
        private readonly string _stableDeviceId;
        private readonly string _nativeDeviceId;
        private MediaCapture? _capture;
        private MediaFrameReader? _reader;
        private int _frameId;
        private int _publishingFrame;

        public CaptureSession(string stableDeviceId, string nativeDeviceId)
        {
            _stableDeviceId = stableDeviceId;
            _nativeDeviceId = nativeDeviceId;
        }

        public async Task StartAsync()
        {
            _capture = new MediaCapture();
            await _capture.InitializeAsync(new MediaCaptureInitializationSettings
            {
                VideoDeviceId = _nativeDeviceId,
                StreamingCaptureMode = StreamingCaptureMode.Video
            });

            var source = _capture.FrameSources.Values.FirstOrDefault(frameSource =>
                frameSource.Info.MediaStreamType == MediaStreamType.VideoPreview &&
                frameSource.Info.SourceKind == MediaFrameSourceKind.Color) ??
                _capture.FrameSources.Values.FirstOrDefault(frameSource =>
                    frameSource.Info.SourceKind == MediaFrameSourceKind.Color);

            if (source is null)
            {
                throw new InvalidOperationException("No color video frame source was exposed by this device.");
            }

            _reader = await _capture.CreateFrameReaderAsync(source, MediaEncodingSubtypes.Bgra8);
            _reader.FrameArrived += OnFrameArrived;
            var status = await _reader.StartAsync();
            if (status != MediaFrameReaderStartStatus.Success)
            {
                throw new InvalidOperationException($"Frame reader did not start: {status}");
            }
        }

        private void OnFrameArrived(MediaFrameReader sender, MediaFrameArrivedEventArgs args)
        {
            if (Interlocked.Exchange(ref _publishingFrame, 1) == 1)
            {
                return;
            }

            try
            {
                using var frame = sender.TryAcquireLatestFrame();
                using var sourceBitmap = frame?.VideoMediaFrame?.SoftwareBitmap;
                if (sourceBitmap is null)
                {
                    return;
                }

                using var bgra = sourceBitmap.BitmapPixelFormat == BitmapPixelFormat.Bgra8
                    ? SoftwareBitmap.Copy(sourceBitmap)
                    : SoftwareBitmap.Convert(sourceBitmap, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied);

                var bytes = new byte[bgra.PixelWidth * bgra.PixelHeight * 4];
                bgra.CopyToBuffer(bytes.AsBuffer());
                CaptureDeviceFrameRouter.Publish(new CaptureDeviceFrame
                {
                    DeviceId = _stableDeviceId,
                    Bgra = bytes,
                    Width = bgra.PixelWidth,
                    Height = bgra.PixelHeight,
                    FrameId = Interlocked.Increment(ref _frameId),
                    TimestampMs = Environment.TickCount64
                });
            }
            finally
            {
                Interlocked.Exchange(ref _publishingFrame, 0);
            }
        }

        public async ValueTask DisposeAsync()
        {
            if (_reader is not null)
            {
                _reader.FrameArrived -= OnFrameArrived;
                try
                {
                    await _reader.StopAsync();
                }
                catch
                {
                    // Best effort during shutdown or failed initialization.
                }

                _reader.Dispose();
                _reader = null;
            }

            _capture?.Dispose();
            _capture = null;
        }
    }
}
