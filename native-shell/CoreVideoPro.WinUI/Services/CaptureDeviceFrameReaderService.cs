using CoreVideoPro.WinUI.Models;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Graphics.Imaging;
using Windows.Media.Capture;
using Windows.Media.Capture.Frames;
using Windows.Media.MediaProperties;

namespace CoreVideoPro.WinUI.Services;

public sealed record CaptureDeviceFormatCandidate(
    int Width,
    int Height,
    double Fps,
    string? Subtype);

public static class CaptureDeviceFormatSelector
{
    public static bool AllowsLateFirstFrame(string? deviceName)
    {
        var normalized = deviceName?.Trim().ToUpperInvariant() ?? string.Empty;
        return normalized.Contains("ELGATO", StringComparison.Ordinal) ||
            normalized.Contains("CAM LINK", StringComparison.Ordinal) ||
            normalized.Contains("CAPTURE", StringComparison.Ordinal) ||
            normalized.Contains("UVC", StringComparison.Ordinal);
    }

    public static double Score(CaptureDeviceFormatCandidate format)
    {
        if (format.Width <= 0 || format.Height <= 0)
        {
            return double.MinValue;
        }

        var area = (double)format.Width * format.Height;
        var fullHdArea = 1920.0 * 1080.0;
        var aspect = format.Height > 0 ? (double)format.Width / format.Height : 0;
        var score = 0.0;

        score += Math.Min(area, fullHdArea) / 1000.0;
        score -= area > fullHdArea ? (area - fullHdArea) / 900.0 : 0;
        score += format.Width == 1920 && format.Height == 1080 ? 420.0 : 0.0;
        score += format.Width == 1280 && format.Height == 720 ? 180.0 : 0.0;
        score += Math.Min(format.Fps, 60) * 8.0;
        score -= Math.Abs(aspect - (16.0 / 9.0)) * 120.0;
        score += SubtypeScore(format.Subtype);
        return score;
    }

    public static double SubtypeScore(string? subtype)
    {
        var normalized = NormalizeSubtype(subtype);
        return normalized switch
        {
            "YUY2" => 96,
            "NV12" => 92,
            "BGRA8" or "BGRA" or "ARGB32" => 76,
            "I420" => 68,
            "P010" => 62,
            "MJPG" or "MJPEG" => 55,
            "RGB24" => 45,
            _ => 0
        };
    }

    public static bool ShouldKeepReaderOnlineAfterFirstFrameTimeout(
        bool allowsLateFirstFrame,
        int candidateIndex,
        int candidateCount) =>
        allowsLateFirstFrame &&
        candidateCount > 0 &&
        candidateIndex == candidateCount - 1;

    public static string NormalizeSubtype(string? subtype)
    {
        var normalized = subtype?.Trim().ToUpperInvariant() ?? string.Empty;
        return normalized switch
        {
            "{30323449-0000-0010-8000-00AA00389B71}" => "I420",
            _ => normalized
        };
    }
}

public sealed class CaptureDeviceFrameReaderService : IDisposable
{
    private readonly object _gate = new();
    private readonly Dictionary<string, CaptureSession> _sessions = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Task<CaptureDeviceFormatTelemetry>> _startingSessions = new(StringComparer.Ordinal);
    private static readonly StrategyBasedComWrappers MemoryBufferComWrappers = new();
    private static readonly SemaphoreSlim MediaCaptureStartupGate = new(1, 1);

    public Task<CaptureDeviceFormatTelemetry> StartAsync(CaptureDevice device)
    {
        lock (_gate)
        {
            if (_sessions.TryGetValue(device.Id, out var existing))
            {
                return Task.FromResult(existing.FormatTelemetry);
            }

            if (_startingSessions.TryGetValue(device.Id, out var pending))
            {
                LaunchLog.Write($"capture: joining in-flight start {device.Id}");
                return pending;
            }

            var session = new CaptureSession(
                device.Id,
                device.NativeDeviceId,
                CaptureDeviceFormatSelector.AllowsLateFirstFrame(device.Name));
            var startTask = StartSessionAsync(device.Id, session);
            _startingSessions[device.Id] = startTask;
            _ = startTask.ContinueWith(
                task =>
                {
                    lock (_gate)
                    {
                        if (_startingSessions.TryGetValue(device.Id, out var pending) &&
                            ReferenceEquals(pending, task))
                        {
                            _startingSessions.Remove(device.Id);
                        }
                    }
                },
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            return startTask;
        }
    }

    private async Task<CaptureDeviceFormatTelemetry> StartSessionAsync(string deviceId, CaptureSession session)
    {
        await MediaCaptureStartupGate.WaitAsync().ConfigureAwait(false);
        try
        {
            LaunchLog.Write($"capture: start gate entered {deviceId}");
            var telemetry = await session.StartAsync().ConfigureAwait(false);
            lock (_gate)
            {
                _sessions[deviceId] = session;
            }

            return telemetry;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"capture: start failed {deviceId}: {DescribeException(ex)}");
            await session.DisposeAsync().ConfigureAwait(false);
            throw;
        }
        finally
        {
            MediaCaptureStartupGate.Release();
        }
    }

    public async Task StopAsync(string deviceId)
    {
        CaptureSession? session;
        lock (_gate)
        {
            if (!_sessions.Remove(deviceId, out session))
            {
                return;
            }
        }

        if (session is null)
        {
            return;
        }

        await session.DisposeAsync().ConfigureAwait(false);
    }

    public void Dispose()
    {
        List<CaptureSession> sessions;
        lock (_gate)
        {
            sessions = _sessions.Values.ToList();
            _sessions.Clear();
            _startingSessions.Clear();
        }

        foreach (var session in sessions)
        {
            session.DisposeAsync().AsTask().GetAwaiter().GetResult();
        }
    }

    public sealed record CaptureDeviceFormatTelemetry(int Width, int Height, int Fps);

    private static string DescribeException(Exception ex) =>
        string.IsNullOrWhiteSpace(ex.Message)
            ? $"{ex.GetType().Name} hr=0x{ex.HResult:X8}"
            : $"{ex.GetType().Name} hr=0x{ex.HResult:X8}: {ex.Message}";

    private sealed class CaptureSession : IAsyncDisposable
    {
        private readonly string _stableDeviceId;
        private readonly string _nativeDeviceId;
        private readonly bool _allowLateFirstFrame;
        private MediaCapture? _capture;
        private MediaFrameReader? _reader;
        private int _frameId;
        private int _publishingFrame;
        private int _loggedStrideFallback;
        private int _loggedFirstPublishedFrame;
        private int _formatWidth;
        private int _formatHeight;
        private int _formatFps;
        private long _lastFrameTimestampMs;
        private long _lastFramePublishFailureLogMs;
        private long _lastFrameHealthLogMs;
        private int _busyDropCount;
        private int _publishedSinceHealthLog;
        private double _measuredFps;
        private int _fpsSampleCount;
        private TaskCompletionSource<CaptureDeviceFormatTelemetry>? _firstFramePublished;

        public CaptureSession(string stableDeviceId, string nativeDeviceId, bool allowLateFirstFrame)
        {
            _stableDeviceId = stableDeviceId;
            _nativeDeviceId = nativeDeviceId;
            _allowLateFirstFrame = allowLateFirstFrame;
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

            var formats = GetRankedFormats(source);
            LaunchLog.Write(
                $"capture: supported formats {_stableDeviceId} {string.Join("; ", formats.Take(8).Select(FormatLabel))}");

            var errors = new List<string>();
            var candidates = BuildStartupCandidates(source, formats);
            for (var i = 0; i < candidates.Count; i++)
            {
                var format = candidates[i];
                CaptureDeviceFormatTelemetry? telemetry = null;
                try
                {
                    telemetry = await TryStartReaderAsync(
                        source,
                        format,
                        CaptureDeviceFormatSelector.ShouldKeepReaderOnlineAfterFirstFrameTimeout(
                            _allowLateFirstFrame,
                            i,
                            candidates.Count)).ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    LaunchLog.Write(
                        $"capture: format failed {_stableDeviceId} {FormatLabel(format)}: {DescribeException(ex)}");
                    await DisposeReaderAsync().ConfigureAwait(false);
                }

                if (telemetry is not null)
                {
                    return telemetry;
                }

                errors.Add(FormatLabel(format));
            }

            throw new InvalidOperationException(
                errors.Count == 0
                    ? "Frame reader did not start for any exposed color format."
                    : $"Frame reader produced no frames after trying: {string.Join(", ", errors)}");
        }

        private void OnFrameArrived(MediaFrameReader sender, MediaFrameArrivedEventArgs args)
        {
            if (Interlocked.Exchange(ref _publishingFrame, 1) == 1)
            {
                Interlocked.Increment(ref _busyDropCount);
                MaybeLogFrameHealth();
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
                    : SoftwareBitmap.Convert(sourceBitmap, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore);
                var bgra = converted ?? sourceBitmap;

                var bytes = CopyBgraBytes(bgra);
                var now = Environment.TickCount64;
                var fps = ResolveFrameRate(now);
                if (Interlocked.Exchange(ref _loggedFirstPublishedFrame, 1) == 0)
                {
                    LaunchLog.Write($"capture: first frame published {_stableDeviceId} {bgra.PixelWidth}x{bgra.PixelHeight} {fps}fps");
                }

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
                _firstFramePublished?.TrySetResult(FormatTelemetry);
                Interlocked.Increment(ref _publishedSinceHealthLog);
                MaybeLogFrameHealth(now);
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

        private void MaybeLogFrameHealth(long? timestampMs = null)
        {
            var now = timestampMs ?? Environment.TickCount64;
            var previous = Interlocked.Read(ref _lastFrameHealthLogMs);
            if (now - previous < 5000 ||
                Interlocked.CompareExchange(ref _lastFrameHealthLogMs, now, previous) != previous)
            {
                return;
            }

            var busyDrops = Interlocked.Exchange(ref _busyDropCount, 0);
            var published = Interlocked.Exchange(ref _publishedSinceHealthLog, 0);
            if (busyDrops > 0 || published > 0)
            {
                LaunchLog.Write($"capture: health {_stableDeviceId} published={published} busyDrops={busyDrops}");
            }
        }

        private byte[] CopyBgraBytes(SoftwareBitmap bitmap)
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
            catch (Exception ex) when (IsStrideCopyFailure(ex))
            {
                if (Interlocked.Exchange(ref _loggedStrideFallback, 1) == 0)
                {
                    LaunchLog.Write($"capture: using stride-aware bitmap copy for {_stableDeviceId} ({ex.GetType().Name}: {ex.Message})");
                }
            }

            using var buffer = bitmap.LockBuffer(BitmapBufferAccessMode.Read);
            using var reference = buffer.CreateReference();
            unsafe
            {
                var byteAccess = (IMemoryBufferByteAccess)MemoryBufferComWrappers.GetOrCreateObjectForComInstance(
                    ((WinRT.IWinRTObject)reference).NativeObject.ThisPtr,
                    CreateObjectFlags.None);
                var hr = byteAccess.GetBuffer(out var data, out var capacity);
                Marshal.ThrowExceptionForHR(hr);
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

        private static bool IsStrideCopyFailure(Exception ex) =>
            ex is ArgumentException argumentException &&
            argumentException.Message.Contains("stride", StringComparison.OrdinalIgnoreCase);

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

        private async Task<CaptureDeviceFormatTelemetry?> TryStartReaderAsync(
            MediaFrameSource source,
            MediaFrameFormat format,
            bool keepOnlineAfterFirstFrameTimeout)
        {
            await DisposeReaderAsync().ConfigureAwait(false);
            Interlocked.Exchange(ref _loggedFirstPublishedFrame, 0);
            _firstFramePublished = new TaskCompletionSource<CaptureDeviceFormatTelemetry>(
                TaskCreationOptions.RunContinuationsAsynchronously);

            if (!ReferenceEquals(format, source.CurrentFormat))
            {
                await source.SetFormatAsync(format).AsTask().ConfigureAwait(false);
                var video = format.VideoFormat;
                LaunchLog.Write(
                    $"capture: selected format {_stableDeviceId} {video?.Width ?? 0}x{video?.Height ?? 0} {FormatFps(format)}fps {format.Subtype}");
            }

            CaptureFormatTelemetry(source);
            _reader = await _capture!.CreateFrameReaderAsync(source, MediaEncodingSubtypes.Bgra8).AsTask().ConfigureAwait(false);
            _reader.FrameArrived += OnFrameArrived;
            var status = await _reader.StartAsync().AsTask().ConfigureAwait(false);
            if (status != MediaFrameReaderStartStatus.Success)
            {
                LaunchLog.Write($"capture: reader start failed {_stableDeviceId} {FormatLabel(format)} status={status}");
                await DisposeReaderAsync().ConfigureAwait(false);
                return null;
            }

            LaunchLog.Write($"capture: started {_stableDeviceId} {_formatWidth}x{_formatHeight} {_formatFps}fps");
            var completed = await Task.WhenAny(_firstFramePublished.Task, Task.Delay(1500)).ConfigureAwait(false);
            if (completed == _firstFramePublished.Task)
            {
                return await _firstFramePublished.Task.ConfigureAwait(false);
            }

            if (keepOnlineAfterFirstFrameTimeout)
            {
                LaunchLog.Write($"capture: no first frame {_stableDeviceId} {FormatLabel(format)}; keeping reader online for late HDMI signal");
                return FormatTelemetry;
            }

            LaunchLog.Write($"capture: no first frame {_stableDeviceId} {FormatLabel(format)}; trying fallback");
            await DisposeReaderAsync().ConfigureAwait(false);
            return null;
        }

        private static IReadOnlyList<MediaFrameFormat> GetRankedFormats(MediaFrameSource source) =>
            source.SupportedFormats
                .Where(format => format.VideoFormat is not null)
                .OrderByDescending(ScoreFormat)
                .ToList();

        private static IReadOnlyList<MediaFrameFormat> BuildStartupCandidates(
            MediaFrameSource source,
            IReadOnlyList<MediaFrameFormat> rankedFormats)
        {
            var candidates = new List<MediaFrameFormat>();
            if (source.CurrentFormat?.VideoFormat is not null)
            {
                candidates.Add(source.CurrentFormat);
            }

            foreach (var format in rankedFormats)
            {
                if (candidates.Any(candidate => SameFormat(candidate, format)))
                {
                    continue;
                }

                candidates.Add(format);
                if (candidates.Count >= 8)
                {
                    break;
                }
            }

            return candidates;
        }

        private static bool SameFormat(MediaFrameFormat left, MediaFrameFormat right) =>
            string.Equals(left.Subtype, right.Subtype, StringComparison.OrdinalIgnoreCase) &&
            (left.VideoFormat?.Width ?? 0) == (right.VideoFormat?.Width ?? 0) &&
            (left.VideoFormat?.Height ?? 0) == (right.VideoFormat?.Height ?? 0) &&
            Math.Abs(ResolveFormatFps(left) - ResolveFormatFps(right)) < 0.01;

        private static double ScoreFormat(MediaFrameFormat format)
        {
            var video = format.VideoFormat;
            if (video is null)
            {
                return double.MinValue;
            }

            var width = (double)video.Width;
            var height = (double)video.Height;
            return CaptureDeviceFormatSelector.Score(new CaptureDeviceFormatCandidate(
                (int)width,
                (int)height,
                ResolveFormatFps(format),
                format.Subtype));
        }

        private static double ResolveFormatFps(MediaFrameFormat format) =>
            format.FrameRate.Denominator > 0
                ? (double)format.FrameRate.Numerator / format.FrameRate.Denominator
                : 0.0;

        private static int FormatFps(MediaFrameFormat format) =>
            (int)Math.Round(ResolveFormatFps(format));

        private static string FormatLabel(MediaFrameFormat format)
        {
            var video = format.VideoFormat;
            return $"{video?.Width ?? 0}x{video?.Height ?? 0}@{FormatFps(format)} {CaptureDeviceFormatSelector.NormalizeSubtype(format.Subtype)}";
        }

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
            if (elapsed <= 0)
            {
                return _measuredFps > 0 ? (int)Math.Round(_measuredFps) : 0;
            }

            var instantFps = Math.Clamp(1000.0 / elapsed, 1.0, 240.0);
            _fpsSampleCount++;
            _measuredFps = _measuredFps <= 0
                ? instantFps
                : (_measuredFps * 0.88) + (instantFps * 0.12);
            return SnapFrameRate(_measuredFps, _fpsSampleCount);
        }

        private static int SnapFrameRate(double fps, int sampleCount)
        {
            if (sampleCount < 8)
            {
                return Math.Clamp((int)Math.Round(fps), 1, 240);
            }

            foreach (var standard in new[] { 24, 25, 30, 50, 60, 120 })
            {
                if (Math.Abs(fps - standard) <= Math.Max(1.5, standard * 0.06))
                {
                    return standard;
                }
            }

            return Math.Clamp((int)Math.Round(fps), 1, 240);
        }

        public async ValueTask DisposeAsync()
        {
            await DisposeReaderAsync().ConfigureAwait(false);

            _capture?.Dispose();
            _capture = null;
        }

        private async Task DisposeReaderAsync()
        {
            if (_reader is null)
            {
                return;
            }

            _reader.FrameArrived -= OnFrameArrived;
            try
            {
                await _reader.StopAsync().AsTask().ConfigureAwait(false);
            }
            catch
            {
                // Best effort during shutdown or failed initialization.
            }

            _reader.Dispose();
            _reader = null;
        }
    }
}

[Guid("5B0D3235-4DBA-4D44-865E-8F1D0E4FD04D")]
[GeneratedComInterface]
internal unsafe partial interface IMemoryBufferByteAccess
{
    [PreserveSig]
    int GetBuffer(out byte* buffer, out uint capacity);
}
