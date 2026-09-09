using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using CoreVideoPro.WinUI;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Windows.Graphics.DirectX.Direct3D11;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// WinRT IDirect3DDevice ↔ native D3D11 bridge for opening DXGI shared handles
/// and presenting them through a <see cref="SwapChainPanel"/>.
///
/// SHARED-SOURCE SAFETY: the core exports each surface as a single-consumer keyed-mutex
/// DXGI shared texture (producer acquires key 0 / writes / releases key 1; consumer
/// acquires key 1 / copies / releases key 0 — a strict ping-pong that serves ONE
/// consumer). When the SAME source is routed to two monitors (e.g. preview AND program),
/// two hosts open the SAME NtHandle. Because all presents run serialized on the UI thread
/// (CompositionTarget.Rendering), the first host to present each vsync always wins the
/// key-1 acquire and the other host's non-blocking acquire perpetually fails -> it freezes.
///
/// To make BOTH present smoothly we decouple keyed-mutex *acquisition* from *presentation*:
/// a process-wide per-handle <see cref="HandleIngest"/> broker (on a single shared device)
/// acquires the producer's keyed mutex at most once per produced frame, copies the shared
/// texture into a device-private "latest frame" texture, and releases the key immediately.
/// Every host then presents from that private copy (a same-device CopyResource), so there is
/// exactly ONE keyed-mutex consumer regardless of how many hosts show the source. All host
/// swap chains live on the one shared device so the private->backbuffer copy is in-device.
/// </summary>
public sealed class Direct3D11InteropService : IDisposable
{
    public enum PresentationPath
    {
        Uninitialized,
        DeviceReady,
        GpuActive,
        CpuFallback
    }

    // Serializes shared device + per-host swap-chain creation, and guards the process-wide
    // ingest map. On a multi-participant join, program/preview/multiview hosts attach
    // near-simultaneously; serializing creation flattens the driver resource peak so each
    // creation sees a stable state.
    private static readonly object CreationGate = new();

    // The ONE D3D device shared by every interop instance. A single process-wide device
    // (a) lets the per-handle ingest broker copy its private "latest frame" texture into any
    // host's swap-chain back buffer (same-device CopyResource), and (b) removes the per-host
    // device churn on tile load/unload that is itself a fail-fast vector. Created once and
    // kept for the app lifetime (never torn down per host) — device churn destabilizes WinUI.
    private static ID3D11Device? s_sharedDevice;
    private static ID3D11DeviceContext? s_sharedContext;
    private static IDirect3DDevice? s_sharedWinrtDevice;
    private static nint s_sharedDevicePointer;

    // ---- Device-loss retirement (beta slice, 2026-09-09) --------------------------------
    // The shared device can DIE under us: a TDR, a driver upgrade mid-show, a hardware
    // fault. Before this, nothing cleared s_sharedDevice, so EnsureDevice kept returning
    // true for a dead device and every host stayed on CPU fallback until the app restarted,
    // with nothing in the log naming the cause.
    //
    // Retirement is by GENERATION, not by clearing fields ad hoc. Every host records the
    // generation it built its swap chain against; a host whose generation is stale rebuilds
    // from scratch on its next present. A late callback carrying an OLD generation cannot
    // resurrect a retired device (RetireDevice is a no-op for a generation that is already
    // retired) and cannot make a stale host believe it is current.
    //
    // THREADING: every present runs on the UI thread (VideoSurfaceHost hooks
    // CompositionTarget.Rendering), and retirement only ever runs from a present or an
    // attach — so retirement is serialized against presents by the UI thread itself, and
    // against creation by CreationGate. Recreation is NOT done inline at loss time: the
    // failing frame drops to CPU fallback immediately and the next composition tick past the
    // backoff deadline creates the device. That keeps the UI thread free of a device-create
    // stall on the same frame that already failed.
    private static long s_deviceGeneration;
    private static readonly DeviceLossPolicy.DeviceRecoveryPolicy s_recoveryPolicy = new();
    private static long s_deviceRetryAfterMs;
    private static bool s_recoveryAbandoned;

    // Observability: what a tester's support bundle must be able to answer — did device loss
    // happen, how many times, what was the removed reason, did recovery work. Mirrored into
    // launch.log (which SupportBundleArchiveBuilder already collects) on every transition.
    private static int s_deviceLossCount;
    private static int s_deviceRecreateCount;
    private static int s_lastRemovedReason;
    private static string? s_lastLossUtc;
    private static string? s_lastRecoveryUtc;

    /// <summary>Process-wide device-loss history, for logs/telemetry/support escalation.</summary>
    internal readonly record struct DeviceLossReport(
        long Generation,
        int LossCount,
        int RecreateCount,
        int LastRemovedReason,
        string? LastLossUtc,
        string? LastRecoveryUtc,
        int ConsecutiveFailures,
        bool Abandoned);

    internal static DeviceLossReport SnapshotDeviceLoss()
    {
        lock (CreationGate)
        {
            return new DeviceLossReport(
                s_deviceGeneration,
                s_deviceLossCount,
                s_deviceRecreateCount,
                s_lastRemovedReason,
                s_lastLossUtc,
                s_lastRecoveryUtc,
                s_recoveryPolicy.ConsecutiveFailures,
                s_recoveryAbandoned);
        }
    }

    /// <summary>True while the device is retired but the ladder has not given up — i.e. GPU
    /// presentation is expected to come back on its own.</summary>
    internal static bool IsAwaitingDeviceRecovery()
    {
        lock (CreationGate)
        {
            return s_sharedDevice is null && s_deviceLossCount > 0 && !s_recoveryAbandoned;
        }
    }

    /// <summary>Operator-initiated retry after the ladder gave up (and the test seam that
    /// restores process-wide state between cases).</summary>
    internal static void ResetDeviceRecoveryBudget()
    {
        lock (CreationGate)
        {
            s_recoveryPolicy.Reset();
            s_recoveryAbandoned = false;
            s_deviceRetryAfterMs = 0;
            LaunchLog.Write("d3d: device recovery budget reset by operator");
        }
    }

    // Per-handle keyed-mutex ingest broker (see class remarks). Keyed by NtHandle and shared
    // across all interop instances. Structural changes are guarded by CreationGate.
    private sealed class HandleIngest
    {
        public ID3D11Texture2D Shared = null!;
        public IDXGIKeyedMutex Mutex = null!;
        public KeyedMutexAcquireSyncThunk? Acquire;
        public ID3D11Texture2D? Private;
        public int Width;
        public int Height;
        public long Generation;
        public int RefCount;
    }

    private static readonly Dictionary<ulong, HandleIngest> s_ingests = new();

    // After an EnsureSwapChain failure under resource pressure, don't retry every vsync
    // (~60x/s) — that churn is itself a crash vector. Back off and stay on the CPU
    // fallback path until the cooldown elapses.
    private const long SwapChainRetryCooldownMs = 750;
    private long _swapChainRetryAfterMs;

    // Throttle for the per-vsync "no device" line while recovery is backing off / abandoned.
    private const long DeviceFailLogIntervalMs = 5_000;
    private long _nextDeviceFailLogMs;

    // Handles blacklisted after a failed open/present. NOTE: this is deliberately per-INSTANCE
    // and per-DEVICE-GENERATION. A handle that could not be opened against the dead device says
    // nothing about the new one (the usual cause of an open failure IS the dying device), so
    // AdoptDeviceGeneration clears it — otherwise recovery would "succeed" onto an empty
    // blacklist-frozen surface and the operator would still see nothing.
    private readonly HashSet<ulong> _invalidHandles = [];

    // The device generation this instance's swap chain / ingest ref were built against.
    // -1 = never attached to any generation.
    private long _deviceGeneration = -1;
    private IDXGISwapChain1? _swapChain;
    private ID3D11Texture2D? _backBuffer;
    private SwapChainPanel? _panel;
    private int _surfaceWidth;
    private int _surfaceHeight;
    private int _panelWidth;
    private int _panelHeight;
    private ulong _lastPresentedHandle;
    private long _presentCount;
    // The handle whose ingest this instance currently holds a ref on (0 = none). Used to
    // release the ingest ref when the presented handle changes or the host is disposed.
    private ulong _ingestHandle;
    // The ingest generation this instance last presented. Skip-present when unchanged so an
    // idle source doesn't re-present every vsync (the known-stable behavior).
    private long _lastPresentedGeneration = -1;

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int KeyedMutexAcquireSyncThunk(IntPtr self, ulong key, uint dwMilliseconds);

    // IDXGIKeyedMutex::AcquireSync lives at vtable slot 8:
    // IUnknown(0-2) + IDXGIObject(3-6) + IDXGIDeviceSubObject.GetDevice(7) + AcquireSync(8).
    private static KeyedMutexAcquireSyncThunk? BuildAcquireSyncThunk(IDXGIKeyedMutex mutex)
    {
        try
        {
            var self = mutex.NativePointer;
            var vtbl = Marshal.ReadIntPtr(self);
            var fnPtr = Marshal.ReadIntPtr(vtbl, 8 * IntPtr.Size);
            return Marshal.GetDelegateForFunctionPointer<KeyedMutexAcquireSyncThunk>(fnPtr);
        }
        catch
        {
            return null;
        }
    }

    private static long s_nextDiagnosticId;
    private readonly long _diagnosticId = System.Threading.Interlocked.Increment(ref s_nextDiagnosticId);
    private PresentationStageWatchdog? _stageWatchdog;
    private bool _presenting;
    private bool _disposed;
    private PresentationPath _path = PresentationPath.Uninitialized;

    public event Action? PresentationPathChanged;

    // DIAGNOSTIC (preview-freeze): a short label (the host's SurfaceKey) so the present
    // heartbeat shows WHICH surface advanced (preview / program / multiview / tile).
    public string? Label { get; set; }

    public bool IsReady => s_sharedDevice is not null && _swapChain is not null && _backBuffer is not null;

    public bool IsGpuPresenting => _path == PresentationPath.GpuActive;

    public bool IsCpuFallback => _path == PresentationPath.CpuFallback;

    public PresentationPath ActivePath => _path;

    public nint DevicePointer => s_sharedDevicePointer;

    public bool TryAttachSwapChainPanel(SwapChainPanel panel)
    {
        if (_disposed)
        {
            return false;
        }

        DetachPanelHandlers();
        _panel = panel;
        _panel.SizeChanged += OnPanelSizeChanged;
        if (!EnsureDevice())
        {
            // A host that attaches DURING a device-loss backoff must still hook its present
            // loop, or it is stranded on CPU forever even after the device comes back — the
            // per-vsync present is what drives recovery. Report success so the caller hooks
            // CompositionTarget.Rendering; IsReady stays false and presents fall to CPU until
            // EnsureDevice succeeds on a later tick.
            if (IsAwaitingDeviceRecovery())
            {
                LaunchLog.Write($"d3d: attach deferred — awaiting device recovery [{Label}] {SnapshotDeviceLoss()}");
                return true;
            }

            LaunchLog.Write("d3d: device init failed");
            return false;
        }

        if (!EnsureSwapChain())
        {
            LaunchLog.Write("d3d: swap-chain attach failed");
            return false;
        }

        LaunchLog.Write($"d3d: swap-chain attached path={_path}");
        return true;
    }

    public bool TryPresentSharedTexture(SharedTextureHandle handle)
    {
        if (_disposed || _presenting) return false;
        _presenting = true;
        _stageWatchdog ??= new PresentationStageWatchdog((stage, elapsed) =>
            LaunchLog.Write($"d3d: stalled presentation host={_diagnosticId} stage={stage} elapsedMs={elapsed}"));
        try { return TryPresentSharedTextureCore(handle); }
        finally { _stageWatchdog.Mark(null); _presenting = false; }
    }

    private bool TryPresentSharedTextureCore(SharedTextureHandle handle)
    {
        if (_disposed ||
            !handle.IsValid ||
            SharedTextureInteropRules.IsStubHandle(handle.NtHandle) ||
            IsHandleInvalidated(handle.NtHandle))
        {
            LaunchLog.Write($"d3d: present skip (disposed={_disposed} valid={handle.IsValid} " +
                $"stub={SharedTextureInteropRules.IsStubHandle(handle.NtHandle)} invalidated={IsHandleInvalidated(handle.NtHandle)}) 0x{handle.NtHandle:X}");
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }

        _stageWatchdog?.Mark("ensure-device");
        if (!EnsureDevice())
        {
            // Throttled: after a loss this is a DURABLE state (waiting out the backoff, or the
            // ladder gave up), and this runs every vsync — an unthrottled line would be 60
            // log entries a second and would roll the loss diagnosis out of the bundle.
            var now = Environment.TickCount64;
            if (now >= _nextDeviceFailLogMs)
            {
                _nextDeviceFailLogMs = now + DeviceFailLogIntervalMs;
                LaunchLog.Write($"d3d: present skip — EnsureDevice failed [{Label}] {SnapshotDeviceLoss()}");
            }
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }

        _stageWatchdog?.Mark("ensure-swap-chain");
        if (!EnsureSwapChain(handle.Width, handle.Height))
        {
            LaunchLog.Write($"d3d: present skip — EnsureSwapChain failed {handle.Width}x{handle.Height}");
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }

        try
        {
            // Resolve (and ref) the process-wide ingest for this handle. When the presented
            // handle changes, release the old ref so an unused ingest can be reclaimed.
            if (_ingestHandle != handle.NtHandle)
            {
                ReleaseIngestRef(_ingestHandle);
                _ingestHandle = 0;
                _lastPresentedGeneration = -1;
            }

            _stageWatchdog?.Mark("acquire-ingest");
            var ingest = AcquireIngest(handle);
            if (ingest is null)
            {
                SetPresentationPath(PresentationPath.CpuFallback);
                return false;
            }
            _ingestHandle = handle.NtHandle;

            // Pump the keyed mutex at most once per produced frame, across ALL hosts: the
            // first host this vsync to find a new frame (AcquireSync key 1 succeeds) copies
            // the shared texture into the ingest's private "latest frame" texture and
            // releases key 0 immediately — handing the key straight back to the producer.
            // A second host the same vsync sees AcquireSync fail (no newer frame) and simply
            // presents the private copy. So there is exactly ONE keyed-mutex consumer no
            // matter how many hosts show the source -> neither host starves.
            _stageWatchdog?.Mark("ingest-shared-copy");
            PumpIngest(ingest);

            // Skip-present when this host already showed the current frame (idle source):
            // re-presenting the same backbuffer every vsync across N hosts destabilizes WinUI.
            if (_lastPresentedGeneration == ingest.Generation || ingest.Private is null)
            {
                SetPresentationPath(PresentationPath.GpuActive);
                return true;
            }

            // Defensive: a concurrent teardown can null these out between the EnsureSwapChain
            // check above and here. Presenting onto a torn-down swap chain / context is a
            // native fail-fast, so bail to the CPU path instead.
            if (_disposed || s_sharedContext is null || _backBuffer is null || _swapChain is null)
            {
                SetPresentationPath(PresentationPath.CpuFallback);
                return false;
            }

            // Same-device copy from the ingest's private latest-frame texture into our back
            // buffer, then present. No keyed mutex is touched here, so two hosts presenting
            // the same source never contend.
            _stageWatchdog?.Mark("copy-back-buffer");
            s_sharedContext.CopyResource(_backBuffer, ingest.Private);
            _stageWatchdog?.Mark("present");
            var result = _swapChain.Present(1, PresentFlags.DoNotWait);
            if (!PresentationAttempt.Commit(result.Code, () =>
                {
                    _lastPresentedHandle = handle.NtHandle;
                    _lastPresentedGeneration = ingest.Generation;
                }))
            {
                // Keep the prior GPU image if there is one, without claiming
                // this newer generation was presented. Retry on the next tick.
                return _path == PresentationPath.GpuActive;
            }
            SetPresentationPath(PresentationPath.GpuActive);
            // Present runs only on new frames; log a heartbeat every 120 presents so the
            // launch log shows BOTH hosts' present # advancing when they share a handle.
            if (++_presentCount % 120 == 0)
            {
                LaunchLog.WriteVerbose($"d3d: present #{_presentCount} [{Label}] 0x{handle.NtHandle:X} {handle.Width}x{handle.Height}");
                // Forgive the recovery budget once the CURRENT device has presented healthily
                // for long enough. Checked on the heartbeat (~2s at 60fps), never per-vsync —
                // the policy takes a lock and this path is the hot one.
                s_recoveryPolicy.RecordHealthy(DateTimeOffset.UtcNow);
            }
            return true;
        }
        catch (Exception ex)
        {
            // Distinguish "the DEVICE is gone" from "this present did not commit". Two
            // independent signals, because a wrapped/marshalled exception may not carry a
            // recognizable HRESULT: the thrown HRESULT itself, and GetDeviceRemovedReason —
            // which is also the only thing that names the cause in a tester's log.
            var observedGeneration = _deviceGeneration;
            var removedReason = QueryRemovedReason();
            var deviceLost = DeviceLossPolicy.IsDeviceLoss(ex.HResult) ||
                             DeviceLossPolicy.IsRemovedReasonFatal(removedReason);

            LaunchLog.Write(
                $"d3d: present FAILED 0x{handle.NtHandle:X} {handle.Width}x{handle.Height}: {ex.GetType().Name}: " +
                $"{ex.Message} hr=0x{ex.HResult:X8} deviceLoss={deviceLost}");

            ReleaseIngestRef(_ingestHandle);
            _ingestHandle = 0;
            _lastPresentedGeneration = -1;

            if (deviceLost)
            {
                // Do NOT blacklist the handle: it is valid, the device under it is not.
                // Retirement disposes the ingest map wholesale, so no per-handle dispose here.
                RetireDevice(observedGeneration, removedReason, $"present[{Label}]");
                ResetSwapChain();
            }
            else
            {
                DisposeIngest(handle.NtHandle);
                InvalidateSharedHandle(handle.NtHandle);
                ResetSwapChain();
            }

            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }
    }

    // Look up or create the per-handle ingest, taking a ref for this instance. The shared
    // texture is opened once per handle (not once per host) so the keyed mutex has a single
    // consumer. Returns null on failure (caller falls back to CPU).
    private HandleIngest? AcquireIngest(SharedTextureHandle handle)
    {
        lock (CreationGate)
        {
            if (_disposed || s_sharedDevice is null)
            {
                return null;
            }

            if (!s_ingests.TryGetValue(handle.NtHandle, out var ingest))
            {
                ID3D11Texture2D? shared = null;
                IDXGIKeyedMutex? mutex = null;
                try
                {
                    shared = s_sharedDevice.OpenSharedResource<ID3D11Texture2D>((IntPtr)handle.NtHandle);
                    mutex = shared.QueryInterface<IDXGIKeyedMutex>();
                    ingest = new HandleIngest
                    {
                        Shared = shared,
                        Mutex = mutex,
                        Acquire = BuildAcquireSyncThunk(mutex),
                        Width = handle.Width,
                        Height = handle.Height,
                        Generation = 0
                    };
                    s_ingests[handle.NtHandle] = ingest;
                }
                catch (Exception ex)
                {
                    LaunchLog.Write($"d3d: ingest open FAILED 0x{handle.NtHandle:X}: {ex.GetType().Name}: {ex.Message}");
                    try { mutex?.Dispose(); } catch { }
                    try { shared?.Dispose(); } catch { }
                    return null;
                }
            }

            // Take a ref only on the transition to owning this handle (the caller releases the
            // prior handle's ref before calling), so steady-state per-vsync presents don't
            // inflate the count.
            if (_ingestHandle != handle.NtHandle)
            {
                ingest.RefCount++;
            }
            return ingest;
        }
    }

    // Acquire the producer's keyed mutex non-blocking; if a new frame is available, copy it
    // into the ingest's private latest-frame texture and bump the generation, then release
    // key 0 so the producer can write the next frame. No-op when no new frame is ready.
    private void PumpIngest(HandleIngest ingest)
    {
        if (ingest.Acquire is null || s_sharedContext is null || s_sharedDevice is null)
        {
            return;
        }

        // hr != S_OK => the producer hasn't released a new frame (or another host already
        // ingested it this vsync): keep the current private copy. The ONE exception is a
        // device-loss HRESULT — a dead device makes AcquireSync fail forever, which would
        // otherwise look exactly like "no new frame" and freeze the surface silently. Throw
        // so it lands in the present catch and is handled as the device loss it is.
        var acquired = ingest.Acquire(ingest.Mutex.NativePointer, 1, 0);
        if (acquired != 0)
        {
            if (DeviceLossPolicy.IsDeviceLoss(acquired))
            {
                Marshal.ThrowExceptionForHR(acquired);
            }
            return;
        }

        try
        {
            ingest.Private ??= s_sharedDevice.CreateTexture2D(new Texture2DDescription
            {
                Width = (uint)Math.Max(1, ingest.Width),
                Height = (uint)Math.Max(1, ingest.Height),
                MipLevels = 1,
                ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm,
                SampleDescription = new SampleDescription(1, 0),
                Usage = ResourceUsage.Default,
                BindFlags = BindFlags.ShaderResource,
                CPUAccessFlags = CpuAccessFlags.None,
                MiscFlags = ResourceOptionFlags.None
            });

            s_sharedContext.CopyResource(ingest.Private, ingest.Shared);
            ingest.Generation++;
        }
        finally
        {
            // Release key 0 immediately (matches the producer's AcquireSync(0)) so the key
            // round-trips in ~1ms, never held across a vsync Present.
            try { ingest.Mutex.ReleaseSync(0); } catch { }
        }
    }

    private static void ReleaseIngestRef(ulong ntHandle)
    {
        if (ntHandle == 0)
        {
            return;
        }

        lock (CreationGate)
        {
            if (s_ingests.TryGetValue(ntHandle, out var ingest))
            {
                if (--ingest.RefCount <= 0)
                {
                    s_ingests.Remove(ntHandle);
                    DisposeIngestInstance(ingest);
                }
            }
        }
    }

    private static void DisposeIngest(ulong ntHandle)
    {
        lock (CreationGate)
        {
            if (s_ingests.TryGetValue(ntHandle, out var ingest))
            {
                s_ingests.Remove(ntHandle);
                DisposeIngestInstance(ingest);
            }
        }
    }

    private static void DisposeIngestInstance(HandleIngest ingest)
    {
        ingest.Acquire = null;
        try { ingest.Private?.Dispose(); } catch { }
        try { ingest.Mutex?.Dispose(); } catch { }
        try { ingest.Shared?.Dispose(); } catch { }
        ingest.Private = null;
    }

    public void InvalidateSharedHandle(ulong ntHandle)
    {
        if (ntHandle == 0)
        {
            return;
        }

        _invalidHandles.Add(ntHandle);
        if (_lastPresentedHandle == ntHandle)
        {
            _lastPresentedHandle = 0;
        }
    }

    public void Dispose()
    {
        _stageWatchdog?.Dispose();
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        DetachPanelHandlers();
        ReleaseIngestRef(_ingestHandle);
        _ingestHandle = 0;
        ResetSwapChain();
        // NOTE: the shared device is intentionally NOT torn down here — it is process-wide
        // and outlives individual hosts (device churn on host unload fail-fasts WinUI).
        _invalidHandles.Clear();
        _lastPresentedHandle = 0;
        _lastPresentedGeneration = -1;
        _deviceGeneration = -1;
        SetPresentationPath(PresentationPath.Uninitialized);
    }

    private bool IsHandleInvalidated(ulong ntHandle) => _invalidHandles.Contains(ntHandle);

    private void DetachPanelHandlers()
    {
        if (_panel is null)
        {
            return;
        }

        _panel.SizeChanged -= OnPanelSizeChanged;
        _panel = null;
    }

    private void OnPanelSizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (_disposed || _panel is null)
        {
            return;
        }

        var width = Math.Max(1, (int)Math.Ceiling(e.NewSize.Width));
        var height = Math.Max(1, (int)Math.Ceiling(e.NewSize.Height));
        if (width == _panelWidth && height == _panelHeight)
        {
            return;
        }

        _panelWidth = width;
        _panelHeight = height;

        if (_surfaceWidth > 0 && _surfaceHeight > 0)
        {
            // The swap chain is sized to the source (e.g. 1920x1080); don't resize
            // it, just rescale it to fill the new panel size.
            ApplyPanelTransform();
            return;
        }

        EnsureSwapChain(width, height);
    }

    // The composition swap chain is created at the SOURCE size and composited into
    // the SwapChainPanel 1:1 at the origin — so a 1920x1080 surface in a smaller
    // panel shows only a corner (the "super zoomed in" program). Scale the swap
    // chain to fill the panel's layout rect; the panel then applies its own DPI
    // composition scale. Source and panel are both 16:9 (AspectRatioHost), so this
    // fits without distortion.
    private void ApplyPanelTransform()
    {
        if (_panel is null || _swapChain is null || _surfaceWidth <= 0 || _surfaceHeight <= 0)
        {
            return;
        }

        var panelWidth = _panel.ActualWidth;
        var panelHeight = _panel.ActualHeight;
        if (panelWidth <= 0 || panelHeight <= 0)
        {
            return;
        }

        try
        {
            using var swapChain2 = _swapChain.QueryInterface<IDXGISwapChain2>();
            // UNIFORM (aspect-preserving) fit, then center — letterbox/pillarbox. The old
            // code scaled X and Y independently (panelW/surfaceW, panelH/surfaceH), which
            // STRETCHED the video whenever the panel aspect != source aspect — that's the
            // multiviewer distortion (16:9 source in a non-16:9 grid cell). Fit by the
            // smaller scale and center the remainder so circles stay circles.
            var scale = (float)Math.Min(panelWidth / _surfaceWidth, panelHeight / _surfaceHeight);
            var offsetX = (float)((panelWidth - _surfaceWidth * scale) / 2.0);
            var offsetY = (float)((panelHeight - _surfaceHeight * scale) / 2.0);
            swapChain2.MatrixTransform = new System.Numerics.Matrix3x2(scale, 0f, 0f, scale, offsetX, offsetY);
            LaunchLog.Write($"d3d: panel transform panel={panelWidth:F0}x{panelHeight:F0} surface={_surfaceWidth}x{_surfaceHeight} uniformScale={scale:F3} offset={offsetX:F0},{offsetY:F0}");
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"d3d: panel transform failed: {ex.GetType().Name}: {ex.Message}");
        }
    }

    private bool EnsureDevice()
    {
        // Fast path: a live device AND this instance is already bound to the current
        // generation. A stale generation falls through so the instance rebuilds against
        // the new device before it touches a swap chain or an ingest.
        if (s_sharedDevice is not null && s_sharedContext is not null &&
            _deviceGeneration == System.Threading.Interlocked.Read(ref s_deviceGeneration))
        {
            return true;
        }

        // Serialize device creation across instances (see CreationGate) so a
        // multi-host attach burst doesn't race to create the shared device — and so a
        // recreation after a loss cannot race presents from the other hosts.
        lock (CreationGate)
        {
            if (_disposed)
            {
                return false;
            }

            if (s_sharedDevice is not null && s_sharedContext is not null)
            {
                AdoptDeviceGeneration();
                return true;
            }

            // The device is gone (retired after a loss, or never created). Recreation is
            // BOUNDED by the recovery ladder: a wedged GPU must not become a per-vsync
            // recreate loop — that churn is itself a 0xc000027b vector (CLAUDE.md).
            if (s_recoveryAbandoned)
            {
                return false;
            }

            if (s_deviceRetryAfterMs != 0 && Environment.TickCount64 < s_deviceRetryAfterMs)
            {
                return false;
            }

            try
            {
                var device = D3D11.D3D11CreateDevice(
                    DriverType.Hardware,
                    DeviceCreationFlags.BgraSupport);
                var context = device.ImmediateContext;

                using var dxgiDevice = device.QueryInterface<IDXGIDevice>();
                var hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.NativePointer, out var winrtDevicePtr);
                if (hr >= 0 && winrtDevicePtr != IntPtr.Zero)
                {
                    s_sharedWinrtDevice = (IDirect3DDevice)Marshal.GetObjectForIUnknown(winrtDevicePtr);
                    s_sharedDevicePointer = winrtDevicePtr;
                    Marshal.Release(winrtDevicePtr);
                }

                s_sharedDevice = device;
                s_sharedContext = context;
                s_deviceRetryAfterMs = 0;
                s_recoveryPolicy.RecordRunning(DateTimeOffset.UtcNow);

                if (s_deviceLossCount > 0)
                {
                    s_deviceRecreateCount++;
                    s_lastRecoveryUtc = DateTimeOffset.UtcNow.ToString("O");
                    LaunchLog.Write(
                        $"d3d: DEVICE RECOVERED generation={s_deviceGeneration} recreates={s_deviceRecreateCount} " +
                        $"losses={s_deviceLossCount} lastRemovedReason=0x{s_lastRemovedReason:X8} — GPU presentation resuming");
                }

                AdoptDeviceGeneration();
                SetPresentationPath(PresentationPath.DeviceReady);
                return true;
            }
            catch (Exception ex)
            {
                // A failed CREATE spends recovery budget exactly like a loss does, so a GPU
                // that can no longer produce a device gives up after a small, bounded number
                // of attempts instead of retrying forever.
                LaunchLog.Write($"d3d: device create FAILED: {ex.GetType().Name}: {ex.Message} hr=0x{ex.HResult:X8}");
                ScheduleRecoveryAttempt("device-create-failed");
                SetPresentationPath(PresentationPath.CpuFallback);
                return false;
            }
        }
    }

    // Bind this instance to the current device generation, discarding everything derived from
    // the previous one. MUST be called under CreationGate.
    private void AdoptDeviceGeneration()
    {
        var current = s_deviceGeneration;
        if (_deviceGeneration == current)
        {
            return;
        }

        if (_deviceGeneration >= 0)
        {
            LaunchLog.Write($"d3d: host={_diagnosticId} [{Label}] adopting device generation {_deviceGeneration} -> {current}");
        }

        _deviceGeneration = current;

        // Swap chain + back buffer belonged to the dead device: drop them so the next
        // EnsureSwapChain builds fresh ones on the new device. This is a CREATE, never a
        // ResizeBuffers — the resize-vs-present race stays impossible by construction.
        ResetSwapChain();

        // The ingest map was disposed wholesale at retirement, so this instance's ref is
        // already gone; just forget it (ReleaseIngestRef would be a no-op anyway).
        _ingestHandle = 0;
        _lastPresentedHandle = 0;
        _lastPresentedGeneration = -1;

        // A handle blacklisted against the dead device is very likely fine against the new
        // one — the dying device is usually WHY the open failed. Start clean.
        _invalidHandles.Clear();

        // Don't inherit the previous device's swap-chain cooldown either.
        _swapChainRetryAfterMs = 0;
    }

    // Retire the shared device as a generation. Returns true if THIS call performed the
    // retirement (a second host observing the same loss is a no-op, not a second budget hit).
    private static bool RetireDevice(long observedGeneration, int removedReason, string context)
    {
        lock (CreationGate)
        {
            if (observedGeneration != s_deviceGeneration)
            {
                // Another host already retired this generation — or this is a late callback
                // from a device that is already gone. Never resurrect anything.
                return false;
            }

            s_deviceGeneration++;
            s_deviceLossCount++;
            s_lastRemovedReason = removedReason;
            s_lastLossUtc = DateTimeOffset.UtcNow.ToString("O");

            // Every cached texture on the dead device is invalid. The ingest map is the only
            // state owned solely here, so it is the only thing disposed outright.
            foreach (var ingest in s_ingests.Values)
            {
                DisposeIngestInstance(ingest);
            }
            s_ingests.Clear();

            // The device/context RCWs are DROPPED, not disposed: other hosts' swap chains and
            // back buffers still hold native references to this device, and disposing an RCW
            // out from under live children is precisely the teardown-order hazard this file
            // already warns about. Each host releases its own swap chain when it adopts the
            // new generation, and the orphaned RCWs finalize after that.
            s_sharedDevice = null;
            s_sharedContext = null;
            s_sharedWinrtDevice = null;
            s_sharedDevicePointer = 0;

            LaunchLog.Write(
                $"d3d: DEVICE LOST context={context} generation={s_deviceGeneration - 1}->{s_deviceGeneration} " +
                $"removedReason=0x{removedReason:X8} ({DeviceLossPolicy.DescribeRemovedReason(removedReason)}) " +
                $"totalLosses={s_deviceLossCount}");

            ScheduleRecoveryAttempt(context);
            return true;
        }
    }

    // Consult the bounded ladder and set (or abandon) the next recreation deadline.
    // MUST be called under CreationGate.
    private static void ScheduleRecoveryAttempt(string context)
    {
        var delay = s_recoveryPolicy.NextDelay(DateTimeOffset.UtcNow);
        if (delay is null)
        {
            s_recoveryAbandoned = true;
            LaunchLog.Write(
                $"d3d: DEVICE RECOVERY ABANDONED after {s_recoveryPolicy.ConsecutiveFailures - 1} consecutive failures " +
                $"context={context} lastRemovedReason=0x{s_lastRemovedReason:X8} " +
                $"({DeviceLossPolicy.DescribeRemovedReason(s_lastRemovedReason)}) — every surface stays on CPU " +
                "fallback until the app is restarted. This is a GPU/driver fault, not a rendering bug.");
            return;
        }

        s_deviceRetryAfterMs = Environment.TickCount64 + (long)delay.Value.TotalMilliseconds;
        LaunchLog.Write(
            $"d3d: device recovery attempt {s_recoveryPolicy.ConsecutiveFailures} scheduled in {delay.Value.TotalMilliseconds:F0}ms context={context}");
    }

    // GetDeviceRemovedReason — the one call that says WHY afterwards. Never throws: a device
    // so far gone that even this fails still has to produce a log line.
    private static int QueryRemovedReason()
    {
        try
        {
            var device = s_sharedDevice;
            return device is null ? DeviceLossPolicy.DeviceRemoved : device.DeviceRemovedReason.Code;
        }
        catch
        {
            return DeviceLossPolicy.DeviceRemoved;
        }
    }

    private bool EnsureSwapChain(int width = 0, int height = 0)
    {
        if (_disposed || _panel is null || s_sharedDevice is null)
        {
            return false;
        }

        // Never build a swap chain against a device this instance has not adopted (e.g. a
        // panel SizeChanged arriving between a retirement and the next present). EnsureDevice
        // adopts the new generation and the next present builds it then.
        if (_deviceGeneration != s_deviceGeneration)
        {
            return false;
        }

        var targetWidth = width > 0 ? width : Math.Max(1, (int)Math.Ceiling(_panel.ActualWidth));
        var targetHeight = height > 0 ? height : Math.Max(1, (int)Math.Ceiling(_panel.ActualHeight));
        if (targetWidth <= 1 || targetHeight <= 1)
        {
            targetWidth = 1280;
            targetHeight = 720;
        }

        _panelWidth = Math.Max(1, (int)Math.Ceiling(_panel.ActualWidth));
        _panelHeight = Math.Max(1, (int)Math.Ceiling(_panel.ActualHeight));

        if (_swapChain is not null && _surfaceWidth == targetWidth && _surfaceHeight == targetHeight)
        {
            return true;
        }

        // Back off after a recent creation failure so we don't churn swap-chain
        // create/teardown every vsync (a crash vector) — stay on CPU until cooldown ends.
        if (_swapChain is null && Environment.TickCount64 < _swapChainRetryAfterMs)
        {
            return false;
        }

        try
        {
            // Serialize creation across all interop instances to flatten the resource
            // spike when 3+ hosts attach at once on a multi-participant join.
            lock (CreationGate)
            {
                if (_disposed)
                {
                    return false;
                }

                ResetSwapChain();

                using var dxgiDevice = s_sharedDevice.QueryInterface<IDXGIDevice>();
                using var adapter = dxgiDevice.GetAdapter();
                using var factory = adapter.GetParent<IDXGIFactory2>();

                var swapChainDesc = new SwapChainDescription1
                {
                    Width = (uint)targetWidth,
                    Height = (uint)targetHeight,
                    Format = Format.B8G8R8A8_UNorm,
                    Stereo = false,
                    SampleDescription = new SampleDescription(1, 0),
                    BufferUsage = Usage.RenderTargetOutput,
                    BufferCount = 2,
                    Scaling = Scaling.Stretch,
                    SwapEffect = SwapEffect.FlipSequential,
                    AlphaMode = AlphaMode.Premultiplied,
                    Flags = SwapChainFlags.None
                };

                _swapChain = factory.CreateSwapChainForComposition(s_sharedDevice, swapChainDesc);
                if (!SwapChainPanelNativeInterop.TrySetSwapChain(_panel, _swapChain.NativePointer, out var attachFailure))
                {
                    LaunchLog.Write($"d3d: panel attach failed: {attachFailure}");
                    ResetSwapChain();
                    _swapChainRetryAfterMs = Environment.TickCount64 + SwapChainRetryCooldownMs;
                    SetPresentationPath(PresentationPath.CpuFallback);
                    return false;
                }

                _backBuffer = _swapChain.GetBuffer<ID3D11Texture2D>(0);
                _surfaceWidth = targetWidth;
                _surfaceHeight = targetHeight;
                _swapChainRetryAfterMs = 0;
                ApplyPanelTransform();
                return true;
            }
        }
        catch
        {
            ResetSwapChain();
            _swapChainRetryAfterMs = Environment.TickCount64 + SwapChainRetryCooldownMs;
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }
    }

    private void ResetSwapChain()
    {
        // COM RCW disposes can throw E_NOINTERFACE during teardown — never let that
        // escape (it fail-fasts the app when a tile/host unloads). The shared device,
        // shared context and per-handle ingests are process-wide and NOT touched here.
        try { _backBuffer?.Dispose(); } catch { }
        try { _swapChain?.Dispose(); } catch { }
        _backBuffer = null;
        _swapChain = null;
        _surfaceWidth = 0;
        _surfaceHeight = 0;
        _lastPresentedGeneration = -1;
    }

    private void SetPresentationPath(PresentationPath path)
    {
        if (_path == path)
        {
            return;
        }

        _path = path;
        PresentationPathChanged?.Invoke();
    }

    [DllImport("d3d11.dll", ExactSpelling = true, PreserveSig = true)]
    private static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);
}
