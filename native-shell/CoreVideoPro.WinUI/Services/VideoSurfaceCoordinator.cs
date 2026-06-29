using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Routes media-core frame metadata to program, preview, and multiview surfaces.
/// Computes FPS locally; shared-texture handles are queued but not presented yet.
/// </summary>
public sealed class VideoSurfaceCoordinator : IDisposable
{
    private const int UiUpdateIntervalMs = 16;

    private readonly object _gate = new();
    private readonly Dictionary<string, FrameRateTracker> _trackers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, VideoSurfaceState> _participantSurfaces = new(StringComparer.Ordinal);
    private readonly Dictionary<string, SharedTextureHandle> _participantSharedHandles = new(StringComparer.Ordinal);
    private readonly Dictionary<string, VideoSurfaceState> _captureDeviceSurfaces = new(StringComparer.Ordinal);
    private readonly Dictionary<string, ZoomVideoFrame> _pendingFrames = new(StringComparer.Ordinal);
    private readonly Dictionary<string, long> _lastUiUpdateMs = new(StringComparer.Ordinal);
    private readonly Dictionary<string, int> _lastAppliedFrameId = new(StringComparer.Ordinal);
    private readonly Timer _uiFlushTimer;
    private VideoSurfaceState _programSurface = VideoSurfaceState.Slate(VideoSurfaceKind.Program, "program", "Program");
    private string _lastProgramSharedSignature = "";
    private VideoSurfaceState _previewSurface = VideoSurfaceState.Slate(VideoSurfaceKind.Preview, "preview", "Preview");
    private string _previewParticipantId = string.Empty;
    private string _compositorRenderer = "software";

    // The media core / compositor is ALWAYS ON. This flag tracks whether the operator has
    // subscribed the raw Zoom capture spine. It only gates per-participant Zoom frames — the
    // always-on program/compositor output is never gated on it.
    private bool _zoomCaptureSubscribed;

    public event Action? SurfacesChanged;

    public VideoSurfaceCoordinator()
    {
        _uiFlushTimer = new Timer(_ => FlushPendingUiUpdates(), null, UiUpdateIntervalMs, UiUpdateIntervalMs);
    }

    public VideoSurfaceState ProgramSurface
    {
        get
        {
            lock (_gate)
            {
                return _programSurface;
            }
        }
    }

    public VideoSurfaceState PreviewSurface
    {
        get
        {
            lock (_gate)
            {
                return _previewSurface;
            }
        }
    }

    public IReadOnlyList<ParticipantSurfaceTile> BuildMultiviewTiles(IReadOnlyList<Participant> participants)
    {
        lock (_gate)
        {
            return participants
                .Select((participant, index) =>
                {
                    var key = ParticipantKey(participant.Id);
                    var surface = _participantSurfaces.TryGetValue(key, out var existing)
                        ? existing
                        : VideoSurfaceState.Waiting(VideoSurfaceKind.Multiview, key, participant.Name);
                    // Prefer the participant's GPU shared texture (smooth multiview)
                    // over the base64 thumbnail when available.
                    if (_participantSharedHandles.TryGetValue(key, out var gpuHandle) && gpuHandle.IsValid)
                    {
                        surface = surface.WithSharedHandle(gpuHandle);
                    }
                    return new ParticipantSurfaceTile
                    {
                        Participant = participant,
                        Surface = surface,
                        SourceIndex = index + 1
                    };
                })
                .ToList();
        }
    }

    public IReadOnlyDictionary<string, VideoSurfaceState> CaptureDeviceSurfaces
    {
        get
        {
            lock (_gate)
            {
                return _captureDeviceSurfaces.ToDictionary(pair => pair.Key, pair => pair.Value, StringComparer.Ordinal);
            }
        }
    }

    public void SetZoomCaptureSubscribed(bool subscribed, string? compositorRenderer = null)
    {
        lock (_gate)
        {
            _zoomCaptureSubscribed = subscribed;
            if (!string.IsNullOrWhiteSpace(compositorRenderer))
            {
                _compositorRenderer = compositorRenderer;
            }

            if (!subscribed)
            {
                // Drop per-participant Zoom frame state, but keep the program surface as-is:
                // the compositor is always on and keeps publishing program frames. The preview
                // (which mirrors a participant feed) falls back to a "capture paused" slate
                // instead of a hard "waiting for compositor output".
                _trackers.Remove("program");
                foreach (var key in _participantSurfaces.Keys.ToList())
                {
                    _trackers.Remove(key);
                }

                _participantSurfaces.Clear();
                _pendingFrames.Clear();
                _lastUiUpdateMs.Clear();
                _lastAppliedFrameId.Clear();
                _compositorRenderer = "software";
                _previewSurface = VideoSurfaceState.CapturePaused(VideoSurfaceKind.Preview, "preview", "Preview");
            }
        }

        NotifyChanged();
    }

    public void SetPreviewParticipant(string? participantId)
    {
        lock (_gate)
        {
            _previewParticipantId = string.IsNullOrWhiteSpace(participantId) ? string.Empty : participantId;
            if (_participantSurfaces.TryGetValue(ParticipantKey(_previewParticipantId), out var participantSurface) &&
                participantSurface.LastFrame is not null)
            {
                _previewSurface = participantSurface with
                {
                    SurfaceKey = "preview",
                    Kind = VideoSurfaceKind.Preview,
                    Title = "Preview"
                };
            }
        }

        NotifyChanged();
    }

    public void OnZoomVideoFrame(ZoomVideoFrame frame)
    {
        // Per-participant raw frames only arrive while capture is subscribed.
        if (!_zoomCaptureSubscribed || frame.Bgra.Length == 0)
        {
            return;
        }

        var trackerKey = ParticipantKey(frame.ParticipantId);
        lock (_gate)
        {
            // Keep the base64 CPU pixels fresh for every participant. The multiview tiles
            // now render on the CPU (no per-tile GPU swap chain) for stability — N
            // concurrent 1080p swap chains churning on a live Zoom join/active-speaker
            // change is what fail-fasts the WinUI (CoreMessagingXP 0xc000027b). The
            // OnSurfacesChanged refresh is rate-throttled (~12.5/s), so feeding base64 to
            // the CPU tiles no longer floods the dispatcher. (Program/preview stay GPU via
            // the core's single composite texture.)
            if (_lastAppliedFrameId.TryGetValue(trackerKey, out var appliedFrameId) &&
                frame.FrameId <= appliedFrameId &&
                !_pendingFrames.ContainsKey(trackerKey))
            {
                return;
            }

            _pendingFrames[trackerKey] = frame;
            if (!TryApplyPendingFrameUnlocked(trackerKey, frame, force: false))
            {
                return;
            }
        }

        NotifyChanged();
    }

    public bool OnCaptureDeviceFrame(CaptureDeviceFrame frame)
    {
        if (frame.Bgra.Length == 0 || frame.Width <= 0 || frame.Height <= 0)
        {
            return false;
        }

        var trackerKey = CaptureDeviceKey(frame.DeviceId);
        var now = Environment.TickCount64;
        lock (_gate)
        {
            if (_lastAppliedFrameId.TryGetValue(trackerKey, out var appliedFrameId) && frame.FrameId <= appliedFrameId)
            {
                return false;
            }

            if (_lastUiUpdateMs.TryGetValue(trackerKey, out var lastUpdateMs) &&
                now - lastUpdateMs < UiUpdateIntervalMs)
            {
                return false;
            }

            var fps = frame.Fps > 0 ? frame.Fps : TrackFps(trackerKey, frame.FrameId);
            var metadata = new VideoFrameMetadata
            {
                ParticipantId = frame.DeviceId,
                Width = frame.Width,
                Height = frame.Height,
                FrameId = frame.FrameId,
                Fps = fps,
                Renderer = "winrt-mediaframe",
                Health = "live",
                TimestampMs = frame.TimestampMs
            };

            _captureDeviceSurfaces[frame.DeviceId] = VideoSurfaceState
                .Waiting(VideoSurfaceKind.Multiview, trackerKey, frame.DeviceId)
                .WithFrame(metadata, "Capture feed live", $"Frame {frame.FrameId} - {metadata.ResolutionLabel} - {metadata.FpsLabel}")
                .WithPreviewPixels(
                    frame.Bgra,
                    frame.Width,
                    frame.Height,
                    frame.NaturalSourceWidth > 0 ? frame.NaturalSourceWidth : frame.Width,
                    frame.NaturalSourceHeight > 0 ? frame.NaturalSourceHeight : frame.Height);
            _lastUiUpdateMs[trackerKey] = now;
            _lastAppliedFrameId[trackerKey] = frame.FrameId;
        }

        NotifyChanged();
        return true;
    }

    public void OnProgramFramePreview(ProgramFramePreview preview)
    {
        // Program frames come from the always-on compositor — never gated on capture.
        if (SharedTextureInteropRules.ShouldPreferGpuSurface(preview.SharedTexture))
        {
            ApplyProgramSharedTexture(
                preview.SharedTexture!,
                preview.RenderPlanId,
                preview.Renderer,
                preview.Health);
        }

        if (preview.Bgra is { Length: > 0 })
        {
            ApplyProgramPreview(preview.FrameNumber, preview.Width, preview.Height, preview.RenderPlanId, preview.Renderer, preview.Health, preview.Bgra);
        }
    }

    public void OnProgramSharedTexture(ProgramSharedTexture texture)
    {
        // Program GPU surfaces come from the always-on compositor — never gated on capture.
        if (!SharedTextureInteropRules.IsPresentable(texture))
        {
            return;
        }

        ApplyProgramSharedTexture(texture);
    }

    public void OnMediaCoreSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        // The compositor is always on, so program frames from every snapshot are accepted
        // regardless of whether the Zoom capture subscription is active.
        if (snapshot.ProgramFramePreview?.Bgra is { Length: > 0 } snapshotPreview)
        {
            ApplyProgramPreview(
                snapshot.ProgramFramePreview.FrameNumber,
                snapshot.ProgramFramePreview.Width,
                snapshot.ProgramFramePreview.Height,
                snapshot.ProgramFramePreview.RenderPlanId,
                snapshot.ProgramFramePreview.Renderer,
                snapshot.ProgramFramePreview.Health,
                snapshotPreview);
        }

        var programFrame = snapshot.ProgramFrame;
        if (programFrame is null && snapshot.ProgramFrameCount <= 0)
        {
            lock (_gate)
            {
                _programSurface = VideoSurfaceState.WaitingForFirstFrame(VideoSurfaceKind.Program, "program", "Program");
            }

            NotifyChanged();
            return;
        }

        var frameNumber = programFrame?.FrameNumber ?? snapshot.ProgramFrameCount;
        var width = programFrame?.Width ?? snapshot.OutputProfile.Width;
        var height = programFrame?.Height ?? snapshot.OutputProfile.Height;
        var fps = programFrame?.Fps ?? snapshot.OutputProfile.Fps;
        var renderer = ResolveRenderer(snapshot);
        var health = programFrame?.Health ?? snapshot.Compositor.Status;
        var renderPlanId = programFrame?.RenderPlanId ?? snapshot.Compositor.RenderPlanId;
        var layerCount = programFrame?.LayerCount ?? 0;

        var trackerKey = "program";
        var measuredFps = TrackFps(trackerKey, frameNumber);
        var metadata = new VideoFrameMetadata
        {
            Width = width,
            Height = height,
            FrameId = frameNumber,
            Fps = measuredFps > 0 ? measuredFps : fps,
            Renderer = renderer,
            Health = health,
            RenderPlanId = renderPlanId,
            LayerCount = layerCount,
            TimestampMs = (long)(programFrame?.TimestampMs ?? snapshot.Diagnostics.GeneratedAtMs)
        };

        var status = DescribeProgramStatus(snapshot.Compositor.Status);
        var detail = $"Frame {frameNumber} · {metadata.ResolutionLabel} · {renderer} · {metadata.FpsLabel}";

        lock (_gate)
        {
            var next = _programSurface.WithFrame(metadata, status, detail);
            _programSurface = next;
        }

        NotifyChanged();
    }

    private static string DescribeProgramStatus(string compositorStatus) =>
        compositorStatus switch
        {
            "failed" or "error" or "dropped" => "Compositor output error",
            "degraded" => "Program compositor degraded",
            "live" => "Program rendering live",
            _ => "Waiting for first compositor frame"
        };

    private void ApplyProgramSharedTexture(
        ProgramSharedTexture texture,
        string? renderPlanId = null,
        string renderer = "d3d11",
        string health = "live")
    {
        var frameNumber = texture.FrameNumber > 0 ? texture.FrameNumber : 0;
        var trackerKey = "program";
        var measuredFps = frameNumber > 0 ? TrackFps(trackerKey, frameNumber) : 0;
        var metadata = new VideoFrameMetadata
        {
            Width = texture.Width,
            Height = texture.Height,
            FrameId = frameNumber,
            Fps = measuredFps,
            Renderer = renderer,
            Health = health,
            RenderPlanId = renderPlanId,
            TimestampMs = Environment.TickCount64
        };

        var handle = ToSharedTextureHandle(texture);
        bool structuralChange;
        lock (_gate)
        {
            var next = _programSurface
                .WithFrame(metadata, "Program GPU surface live", $"Frame {frameNumber} · {metadata.ResolutionLabel} · {renderer}");
            if (health is not "live")
            {
                next = next.WithFrame(metadata, DescribeProgramStatus(health), next.DetailLine);
            }

            if (handle.IsValid)
            {
                next = next.WithSharedHandle(handle);
            }

            _programSurface = next;

            // The compositor reuses the same shared texture across frames, so the
            // handle value is stable; the GPU present runs every vsync off that
            // handle (VideoSurfaceHost). Only fire the heavy surface-binding refresh
            // when something structural actually changes (handle value, size, or
            // health) — not on every 60fps frame, which would bog the UI thread.
            var signature = $"{handle.NtHandle:X}:{texture.Width}x{texture.Height}:{health}";
            structuralChange = signature != _lastProgramSharedSignature;
            _lastProgramSharedSignature = signature;
        }

        if (structuralChange)
        {
            NotifyChanged();
        }
    }

    private void ApplyProgramPreview(
        int frameNumber,
        int width,
        int height,
        string? renderPlanId,
        string renderer,
        string health,
        byte[] bgra)
    {
        var trackerKey = "program";
        var measuredFps = TrackFps(trackerKey, frameNumber);
        var metadata = new VideoFrameMetadata
        {
            Width = width,
            Height = height,
            FrameId = frameNumber,
            Fps = measuredFps,
            Renderer = renderer,
            Health = health,
            RenderPlanId = renderPlanId,
            TimestampMs = Environment.TickCount64
        };

        lock (_gate)
        {
            _programSurface = _programSurface
                .WithFrame(metadata, "Program preview live", $"Frame {frameNumber} · {metadata.ResolutionLabel} · {renderer}")
                .WithPreviewPixels(bgra, width, height);
            if (health is not "live")
            {
                _programSurface = _programSurface
                    .WithFrame(metadata, DescribeProgramStatus(health), _programSurface.DetailLine)
                    .WithPreviewPixels(bgra, width, height);
            }
        }

        NotifyChanged();
    }

    public void Dispose()
    {
        _uiFlushTimer.Dispose();
    }

    private void FlushPendingUiUpdates()
    {
        // Only participant (capture) frames are buffered here.
        if (!_zoomCaptureSubscribed)
        {
            return;
        }

        var changed = false;
        lock (_gate)
        {
            foreach (var participantKey in _pendingFrames.Keys.ToList())
            {
                if (!_pendingFrames.TryGetValue(participantKey, out var frame))
                {
                    continue;
                }

                if (TryApplyPendingFrameUnlocked(participantKey, frame, force: false))
                {
                    changed = true;
                }
            }
        }

        if (changed)
        {
            NotifyChanged();
        }
    }

    private bool TryApplyPendingFrameUnlocked(string trackerKey, ZoomVideoFrame frame, bool force)
    {
        if (_lastAppliedFrameId.TryGetValue(trackerKey, out var appliedFrameId) && frame.FrameId <= appliedFrameId)
        {
            _pendingFrames.Remove(trackerKey);
            return false;
        }

        var now = Environment.TickCount64;
        if (!force &&
            _lastUiUpdateMs.TryGetValue(trackerKey, out var lastUpdateMs) &&
            now - lastUpdateMs < UiUpdateIntervalMs)
        {
            return false;
        }

        var fps = TrackFps(trackerKey, frame.FrameId);
        var metadata = new VideoFrameMetadata
        {
            ParticipantId = frame.ParticipantId,
            Width = frame.Width,
            Height = frame.Height,
            FrameId = frame.FrameId,
            Fps = fps,
            Renderer = "zoom-shm",
            Health = "live",
            TimestampMs = now
        };

        var participantSurface = VideoSurfaceState
            .Waiting(VideoSurfaceKind.Participant, trackerKey, frame.ParticipantId)
            .WithFrame(metadata, "Participant feed live", $"Frame {frame.FrameId} · {metadata.ResolutionLabel} · {metadata.FpsLabel}")
            .WithPreviewPixels(
                frame.Bgra,
                frame.Width,
                frame.Height,
                frame.Width,
                frame.Height);
        _participantSurfaces[trackerKey] = participantSurface;

        if (frame.ParticipantId == _previewParticipantId)
        {
            _previewSurface = participantSurface with
            {
                SurfaceKey = "preview",
                Kind = VideoSurfaceKind.Preview,
                Title = "Preview"
            };
        }

        _lastUiUpdateMs[trackerKey] = now;
        _lastAppliedFrameId[trackerKey] = frame.FrameId;
        _pendingFrames.Remove(trackerKey);
        return true;
    }

    private static string CaptureDeviceKey(string deviceId) => $"capture:{deviceId}";

    private string ResolveRenderer(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.Compositor.Status is "live" or "degraded" && !string.Equals(_compositorRenderer, "software", StringComparison.Ordinal))
        {
            return _compositorRenderer;
        }

        return snapshot.Compositor.Status is "live" or "degraded" ? "gpu-compositor" : _compositorRenderer;
    }

    private double TrackFps(string key, int frameId)
    {
        lock (_gate)
        {
            if (!_trackers.TryGetValue(key, out var tracker))
            {
                tracker = new FrameRateTracker();
                _trackers[key] = tracker;
            }

            return tracker.Record(frameId, Environment.TickCount64);
        }
    }

    private static string ParticipantKey(string participantId) => $"participant:{participantId}";

    private static SharedTextureHandle ToSharedTextureHandle(ProgramSharedTexture texture) =>
        new()
        {
            NtHandle = CoreProtocolParser.ParseSharedHandleHex(texture.SharedHandleHex),
            Width = texture.Width,
            Height = texture.Height,
            Format = texture.Format,
            FrameNumber = texture.FrameNumber
        };

    // Per-participant GPU shared texture for the multiview tiles. The handle value is
    // stable (the core reuses the texture), so only refresh bindings when it changes;
    // the per-tile VideoSurfaceHost then presents each new keyed-mutex frame (skip-present).
    public void OnParticipantSharedTexture(ParticipantSharedTexture texture)
    {
        if (texture is null || !texture.IsValid)
        {
            return;
        }

        var handle = new SharedTextureHandle
        {
            NtHandle = CoreProtocolParser.ParseSharedHandleHex(texture.SharedHandleHex),
            Width = texture.Width,
            Height = texture.Height,
            Format = texture.Format,
            FrameNumber = texture.FrameNumber
        };
        if (!handle.IsValid)
        {
            return;
        }

        var key = ParticipantKey(texture.ParticipantId);
        bool structuralChange;
        lock (_gate)
        {
            var previous = _participantSharedHandles.TryGetValue(key, out var existingHandle) ? existingHandle.NtHandle : 0;
            _participantSharedHandles[key] = handle;
            var surface = _participantSurfaces.TryGetValue(key, out var existing)
                ? existing
                : VideoSurfaceState.Waiting(VideoSurfaceKind.Multiview, key, texture.ParticipantId);
            _participantSurfaces[key] = surface.WithSharedHandle(handle);
            structuralChange = previous != handle.NtHandle;
        }

        if (structuralChange)
        {
            NotifyChanged();
        }
    }

    private void NotifyChanged() => SurfacesChanged?.Invoke();

    private sealed class FrameRateTracker
    {
        private int _lastFrameId;
        private long _lastTimestampMs;
        private double _fps;
        private int _sampleCount;

        public double Record(int frameId, long timestampMs)
        {
            if (_lastTimestampMs > 0 && frameId > _lastFrameId)
            {
                var deltaMs = timestampMs - _lastTimestampMs;
                if (deltaMs > 0)
                {
                    var framesAdvanced = Math.Max(1, frameId - _lastFrameId);
                    var instantFps = Math.Clamp(framesAdvanced * 1000.0 / deltaMs, 1.0, 240.0);
                    _sampleCount++;
                    _fps = _fps <= 0 ? instantFps : (_fps * 0.86) + (instantFps * 0.14);
                }
            }

            _lastFrameId = frameId;
            _lastTimestampMs = timestampMs;
            return SnapFrameRate(_fps, _sampleCount);
        }

        private static double SnapFrameRate(double fps, int sampleCount)
        {
            if (fps <= 0)
            {
                return 0;
            }

            if (sampleCount >= 6)
            {
                foreach (var standard in new[] { 24, 25, 30, 50, 60, 120 })
                {
                    if (Math.Abs(fps - standard) <= Math.Max(1.5, standard * 0.06))
                    {
                        return standard;
                    }
                }
            }

            return Math.Round(fps, 1);
        }
    }
}
