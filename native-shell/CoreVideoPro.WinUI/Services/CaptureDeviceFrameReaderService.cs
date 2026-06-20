using CoreVideoPro.WinUI.Models;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.Capture.Frames;
using Windows.Media.MediaProperties;

namespace CoreVideoPro.WinUI.Services;

public sealed class CaptureDeviceFrameReaderService : IDisposable
{
    private readonly Dictionary<string, CaptureSession> _sessions = new(StringComparer.Ordinal);

    public async Task<CaptureDeviceFormatTelemetry> StartAsync(CaptureDevice device)
    {
        if (_sessions.TryGetValue(device.Id, out var existing))
        {
            return existing.FormatTelemetry;
        }

        var session = new CaptureSession(device.Id, device.NativeDeviceId);
        try
        {
            var telemetry = await session.StartAsync().ConfigureAwait(false);
            _sessions[device.Id] = session;
            return telemetry;
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

    public sealed record CaptureDeviceFormatTelemetry(int Width, int Height, int Fps);

    private sealed class CaptureSession : IAsyncDisposable
    {
        private readonly string _stableDeviceId;
        private readonly string _nativeDeviceId;
        private MediaCapture? _capture;
        private MediaFrameReader? _reader;
        private int _frameId;
        private int _publishingFrame;
        private int _formatWidth;
        private int _formatHeight;
        private int _formatFps;
        private long _lastFrameTimestampMs;
        private long _lastFramePublishFailureLogMs;

        public CaptureSession(string stableDeviceId, string nativeDeviceId)
        {
            _stableDeviceId = stableDeviceId;
            _nativeDeviceId = nativeDeviceId;
        }

        public CaptureDeviceFormatTelemetry FormatTelemetry => new(_formatWidth, _formatHeight, _formatFps);

        public async Task<CaptureDeviceFormatTelemetry> StartAsync()
        {
            _capture = new MediaCapture();
            await _capture.InitializeAsync(new MediaCaptureInitializationSettings
            {
                VideoDeviceId = _nativeDeviceId,
                StreamingCaptureMode = StreamingCaptureMode.Video,
                MemoryPreference = MediaCaptureMemoryPreference.Cpu,
                SharingMode = MediaCaptureSharingMode.SharedReadOnly
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

            CaptureFormatTelemetry(source);

            _reader = await _capture.CreateFrameReaderAsync(source, MediaEncodingSubtypes.Bgra8);
            _reader.FrameArrived += OnFrameArrived;
            var status = await _reader.StartAsync();
            if (status != MediaFrameReaderStartStatus.Success)
            {
                throw new InvalidOperationException($"Frame reader did not start: {status}");
            }

            LaunchLog.Write($"capture: started {_stableDeviceId} {_formatWidth}x{_formatHeight} {_formatFps}fps");
            return FormatTelemetry;
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

                using var converted = sourceBitmap.BitmapPixelFormat == BitmapPixelFormat.Bgra8
                    ? null
                    : SoftwareBitmap.Convert(sourceBitmap, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied);
                var bgra = converted ?? sourceBitmap;

                var bytes = CopyBgraBytes(bgra);
                var now = Environment.TickCount64;
                var fps = ResolveFrameRate(now);
                CaptureDeviceFrameRouter.Publish(new CaptureDeviceFrame
                {
                    DeviceId = _stableDeviceId,
                    Bgra = bytes,
                    Width = bgra.PixelWidth,
                    Height = bgra.PixelHeight,
                    Fps = fps,
                    FrameId = Interlocked.Increment(ref _frameId),
                    TimestampMs = now
                });
            }
            catch (Exception ex)
            {
                var now = Environment.TickCount64;
                var lastLog = Interlocked.Read(ref _lastFramePublishFailureLogMs);
                if (now - lastLog > 2000 && Interlocked.Exchange(ref _lastFramePublishFailureLogMs, now) == lastLog)
                {
                    LaunchLog.Write($"capture: frame publish failed {_stableDeviceId}: {ex.GetType().Name}: {ex.Message}");
                }
            }
            finally
            {
                Interlocked.Exchange(ref _publishingFrame, 0);
            }
        }

        private static byte[] CopyBgraBytes(SoftwareBitmap bitmap)
        {
            var width = bitmap.PixelWidth;
            var height = bitmap.PixelHeight;
            var rowBytes = checked(width * 4);
            var bytes = new byte[checked(rowBytes * height)];

            try
            {
                bitmap.CopyToBuffer(bytes.AsBuffer());
                return bytes;
            }
            catch (ArgumentException ex) when (ex.Message.Contains("Negative stride", StringComparison.OrdinalIgnoreCase))
            {
            }

            using var buffer = bitmap.LockBuffer(BitmapBufferAccessMode.Read);
            using var reference = buffer.CreateReference();
            unsafe
            {
                using var byteAccess = MemoryBufferByteAccess.From(reference);
                byteAccess.Interface.GetBuffer(out var data, out var capacity);
                var plane = buffer.GetPlaneDescription(0);
                var stride = plane.Stride;
                if (stride == 0)
                {
                    throw new InvalidOperationException("Bitmap buffer reported a zero stride.");
                }

                if (!TryCopyRows(data, (int)capacity, plane.StartIndex, stride, rowBytes, height, bytes) &&
                    !TryCopyRows(data, (int)capacity, plane.StartIndex, Math.Abs(stride), rowBytes, height, bytes, flipRows: stride < 0))
                {
                    throw new InvalidOperationException($"Bitmap buffer stride was out of range: start={plane.StartIndex}, stride={stride}, capacity={capacity}.");
                }
            }

            return bytes;
        }

        private static unsafe bool TryCopyRows(
            byte* data,
            int capacity,
            int startIndex,
            int stride,
            int rowBytes,
            int height,
            byte[] destination,
            bool flipRows = false)
        {
            var firstRowOffset = RowOffset(startIndex, stride, height, 0, flipRows);
            var lastRowOffset = RowOffset(startIndex, stride, height, height - 1, flipRows);
            var minOffset = Math.Min(firstRowOffset, lastRowOffset);
            var maxOffset = Math.Max(firstRowOffset, lastRowOffset);
            if (minOffset < 0 || maxOffset + rowBytes > capacity)
            {
                return false;
            }

            for (var y = 0; y < height; y++)
            {
                var sourceOffset = RowOffset(startIndex, stride, height, y, flipRows);
                Marshal.Copy((nint)(data + sourceOffset), destination, y * rowBytes, rowBytes);
            }

            return true;
        }

        private static int RowOffset(int startIndex, int stride, int height, int row, bool flipRows) =>
            startIndex + (flipRows ? height - 1 - row : row) * stride;

        private void CaptureFormatTelemetry(MediaFrameSource source)
        {
            var format = source.CurrentFormat;
            if (format is null)
            {
                return;
            }

            _formatWidth = (int)(format.VideoFormat?.Width ?? 0);
            _formatHeight = (int)(format.VideoFormat?.Height ?? 0);
            if (format.FrameRate.Denominator > 0)
            {
                _formatFps = (int)Math.Round((double)format.FrameRate.Numerator / format.FrameRate.Denominator);
            }
        }

        private int ResolveFrameRate(long timestampMs)
        {
            if (_formatFps > 0)
            {
                return _formatFps;
            }

            var previous = Interlocked.Exchange(ref _lastFrameTimestampMs, timestampMs);
            if (previous <= 0)
            {
                return 0;
            }

            var elapsed = timestampMs - previous;
            return elapsed > 0 ? Math.Clamp((int)Math.Round(1000.0 / elapsed), 1, 240) : 0;
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

[ComImport]
[Guid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal unsafe interface IMemoryBufferByteAccess
{
    void GetBuffer(out byte* buffer, out uint capacity);
}

internal sealed class MemoryBufferByteAccess : IDisposable
{
    private nint _unknown;
    private nint _byteAccess;

    private MemoryBufferByteAccess(nint unknown, nint byteAccess, IMemoryBufferByteAccess instance)
    {
        _unknown = unknown;
        _byteAccess = byteAccess;
        Interface = instance;
    }

    public IMemoryBufferByteAccess Interface { get; }

    public static MemoryBufferByteAccess From(object reference)
    {
        var unknown = Marshal.GetIUnknownForObject(reference);
        var iid = typeof(IMemoryBufferByteAccess).GUID;
        var result = Marshal.QueryInterface(unknown, ref iid, out var byteAccess);
        if (result != 0)
        {
            Marshal.Release(unknown);
            Marshal.ThrowExceptionForHR(result);
        }

        var instance = (IMemoryBufferByteAccess)Marshal.GetTypedObjectForIUnknown(byteAccess, typeof(IMemoryBufferByteAccess));
        return new MemoryBufferByteAccess(unknown, byteAccess, instance);
    }

    public void Dispose()
    {
        if (_byteAccess != 0)
        {
            Marshal.Release(_byteAccess);
            _byteAccess = 0;
        }

        if (_unknown != 0)
        {
            Marshal.Release(_unknown);
            _unknown = 0;
        }
    }
}
