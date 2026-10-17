using System.Collections.Concurrent;
using System.Diagnostics;
using CoreVideoPro.WinUI.Models;
using Microsoft.UI.Xaml.Controls;
using Vortice.DXGI;

namespace CoreVideoPro.WinUI.Services;

/// <summary>One pending request, replaced rather than queued while a driver is busy.</summary>
internal sealed class LatestPresentationRequest<T> where T : class
{
    private readonly object _gate = new();
    private T? _latest;
    private bool _closed;
    public bool Offer(T request)
    {
        lock (_gate)
        {
            if (_closed) return false;
            _latest = request;
            return true;
        }
    }
    public T? Take() { lock (_gate) { var value = _latest; _latest = null; return value; } }
    public void Close() { lock (_gate) { _closed = true; _latest = null; } }
}

/// <summary>
/// XAML only attaches a leased swap chain and publishes immutable geometry/handle
/// requests. Persistent render threads own ALL D3D/DXGI work and retirement. A
/// source and every alias (including Preview) share the same device/ingest broker. No UI wait,
/// per-frame task, unbounded frame queue, CPU video path or per-row device churn.
/// </summary>
internal sealed class VideoSurfaceRenderPresenter : IDisposable
{
    private sealed record Request(SharedTextureHandle Handle, double Width, double Height, bool FillPanel);
    private sealed record FrameState(long Frames, ulong Handle, long Revision, bool Ready, bool Active,
        object? Diagnostics, double MaxGapMs, long SampleTime);
    private sealed record Pixel(ulong Handle, long Revision, uint? Value);
    private readonly SwapChainPanel _panel;
    private readonly Microsoft.UI.Dispatching.DispatcherQueue _dispatcher;
    private readonly RenderThread _renderer;
    private readonly LatestPresentationRequest<Request> _requests = new();
    private Direct3D11InteropService? _interop; // render thread only
    private FrameState? _state;
    private Pixel? _pixel;
    private Request? _current; // render thread only
    private long _requestedHandle;
    private long _latestRevision, _attachedRevision;
    private int _disposed, _active, _readPixel, _resetProbe, _notificationQueued;
    private long _rejectedRevision;
    private long _retryAfter;
    internal string SourceKey { get; }
    internal ulong HandleKey { get; }
    internal event Action? Changed;
    internal static void ResetWorkerRecoveryBudgets() => RenderThread.ResetRecoveryBudgets();

    internal VideoSurfaceRenderPresenter(SwapChainPanel panel, string sourceKey, ulong handleKey)
    {
        _panel = panel; _dispatcher = panel.DispatcherQueue;
        SourceKey = sourceKey; HandleKey = handleKey;
        _renderer = RenderThread.Acquire(handleKey);
        _renderer.Post(() => _renderer.Hosts.Add(this));
    }

    internal bool IsReady => Volatile.Read(ref _state)?.Ready == true;
    internal bool IsPresenting(ulong handle)
    {
        var state = Volatile.Read(ref _state);
        return Volatile.Read(ref _disposed) == 0 && handle != 0 && state is { Active: true, Frames: > 0 } &&
            state.Handle == handle && state.Revision == Volatile.Read(ref _attachedRevision) &&
            state.Revision == Volatile.Read(ref _latestRevision);
    }
    internal long PresentedFrames => Volatile.Read(ref _state)?.Frames ?? 0;
    internal object? Diagnostics => Volatile.Read(ref _state)?.Diagnostics;
    internal double MaximumGapMs
    {
        get
        {
            var state = Volatile.Read(ref _state);
            return state is null || state.SampleTime == 0 ? 0 : Math.Max(state.MaxGapMs,
                Stopwatch.GetElapsedTime(state.SampleTime).TotalMilliseconds);
        }
    }
    internal void BeginProbeInterval() { Interlocked.Exchange(ref _resetProbe, 1); _renderer.Wake(); }
    internal uint? ReadPixelForProbe()
    {
        Interlocked.Exchange(ref _readPixel, 1); _renderer.Wake();
        var pixel = Volatile.Read(ref _pixel);
        return pixel is not null && pixel.Handle == unchecked((ulong)Volatile.Read(ref _requestedHandle)) &&
            pixel.Revision == Volatile.Read(ref _attachedRevision) ? pixel.Value : null;
    }
    internal void Submit(SharedTextureHandle handle, bool fillPanel)
    {
        if (Volatile.Read(ref _disposed) != 0) return;
        Volatile.Write(ref _requestedHandle, unchecked((long)handle.NtHandle));
        if (_requests.Offer(new(handle, Math.Max(1, _panel.ActualWidth),
                Math.Max(1, _panel.ActualHeight), fillPanel)))
        {
            if (Interlocked.Exchange(ref _active, 1) == 0 && Direct3D11InteropService.MonitorDiagnosticsEnabled)
                LaunchLog.Write($"monitor-activity: active [{SourceKey}]");
            _renderer.Wake();
        }
    }
    internal void Suspend()
    {
        if (Interlocked.Exchange(ref _active, 0) != 0 && Direct3D11InteropService.MonitorDiagnosticsEnabled)
            LaunchLog.Write($"monitor-activity: suspended [{SourceKey}]");
        _renderer.Wake();
    }
    private bool Active => Volatile.Read(ref _disposed) == 0 && Volatile.Read(ref _active) != 0;
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        _requests.Close(); Changed = null;
        RenderThread.Release(HandleKey, _renderer);
        _renderer.Wake(); // Retirement happens on the owning thread, never via a UI join.
    }

    private void AttachChain(IDXGISwapChain1 lease, long revision)
    {
        Volatile.Write(ref _latestRevision, revision);
        // QueryInterface/AddRef/Release are safe across threads. The queued UI
        // callback owns this lease even if resize/unload retires its producer.
        if (!_dispatcher.TryEnqueue(() =>
        {
            using (lease)
            {
                if (Volatile.Read(ref _disposed) != 0 || revision != Volatile.Read(ref _latestRevision)) return;
                if (SwapChainPanelNativeInterop.TrySetSwapChain(_panel, lease.NativePointer, out var reason))
                {
                    Volatile.Write(ref _attachedRevision, revision);
                    NotifyUi();
                }
                else
                {
                    LaunchLog.Write($"source-monitor: attach failed [{SourceKey}]: {reason}");
                    Volatile.Write(ref _rejectedRevision, revision); _renderer.Wake();
                }
            }
        })) lease.Dispose();
    }
    private void NotifyUi()
    {
        if (Volatile.Read(ref _disposed) != 0 || Interlocked.Exchange(ref _notificationQueued, 1) != 0) return;
        if (!_dispatcher.TryEnqueue(() =>
        {
            Interlocked.Exchange(ref _notificationQueued, 0);
            if (Volatile.Read(ref _disposed) == 0) Changed?.Invoke();
        })) Interlocked.Exchange(ref _notificationQueued, 0);
    }
    // Invoked exclusively by our assigned persistent render thread.
    private bool Pump()
    {
        if (Volatile.Read(ref _disposed) != 0)
        {
            _interop?.Dispose(); _interop = null; return false;
        }
        try
        {
            if (Interlocked.Exchange(ref _resetProbe, 0) != 0)
            {
                _interop?.BeginPresentationProbeInterval();
            }
            var request = _requests.Take();
            if (request is not null) _current = request;
            if (Environment.TickCount64 < _retryAfter) return true;
            if (_interop is not null && Interlocked.Exchange(ref _rejectedRevision, 0) is var rejected && rejected != 0)
                _interop.RejectWorkerChain(rejected);
            // The handle is stable while its producer publishes new GPU pixels.
            // A fresh XAML request is configuration, not permission for one frame.
            if (Active && _current is { } currentRequest)
            {
                if (_interop is null)
                {
                    _interop = new Direct3D11InteropService { Label = SourceKey };
                    _interop.AttachWorkerPresenter(AttachChain);
                }
                _interop.UpdateWorkerPanelSize(currentRequest.Width, currentRequest.Height, currentRequest.FillPanel);
                _interop.TryPresentSharedTexture(currentRequest.Handle);
            }
            if (_interop is not null && Interlocked.Exchange(ref _readPixel, 0) != 0 && _current is { } current)
                Volatile.Write(ref _pixel, new(current.Handle.NtHandle, _interop.ChainRevision, _interop.ReadPixelForProbe()));
            if (_interop is not null)
            {
                var old = Volatile.Read(ref _state);
                var state = new FrameState(_interop.PresentedFramesForProbe, _interop.LastPresentedHandle,
                    _interop.ChainRevision, _interop.IsReady, _interop.IsGpuPresenting,
                    Direct3D11InteropService.CollectProbeCounters ? _interop.DiagnosticsForProbe : null,
                    _interop.MaximumPresentationProbeGapMs, _interop.LastPresentationProbeCommit);
                Volatile.Write(ref _state, state);
                if (old is null || old.Revision != state.Revision || old.Handle != state.Handle || old.Active != state.Active)
                    NotifyUi();
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"source-monitor: render failure [{SourceKey}] {ex.GetType().Name} hr=0x{ex.HResult:X8}");
            _interop?.Dispose(); _interop = null; Volatile.Write(ref _state, null);
            _retryAfter = Environment.TickCount64 + 750; NotifyUi();
        }
        return true;
    }

    private sealed class RenderThread
    {
        // Route by shared HANDLE, not by UI label: Preview can alias a source.
        // All aliases use ONE keyed-mutex consumer, including during source
        // switches. Devices persist across view churn; requests are not queued.
        private static readonly RenderThread[] Pool = Enumerable.Range(0, 4).Select(index => new RenderThread(index)).ToArray();
        private static readonly SharedTextureRenderAssignments Assignments = new(4);
        private readonly int _index;
        private readonly AutoResetEvent _wake = new(false);
        private readonly ConcurrentQueue<Action> _commands = new();
        internal readonly List<VideoSurfaceRenderPresenter> Hosts = new(); // owning thread only
        private RenderThread(int index)
        {
            _index = index;
            new Thread(Run) { IsBackground = true, Name = "CoreVideo Video Monitor" }.Start();
        }
        internal static RenderThread Acquire(ulong handleKey) => Pool[Assignments.Acquire(handleKey)];
        internal static void Release(ulong handleKey, RenderThread thread) => Assignments.Release(handleKey, thread._index);
        internal static void ResetRecoveryBudgets()
        {
            foreach (var thread in Pool) thread.Post(Direct3D11InteropService.ResetDeviceRecoveryBudget);
        }
        internal void Wake() => _wake.Set();
        internal void Post(Action command) { _commands.Enqueue(command); Wake(); }
        private void Run()
        {
            Direct3D11InteropService.InitializeRenderThread();
            var cadence = new PresentationCadence();
            using var timer = new MonitorRenderTimer(_wake);
            while (true)
            {
                while (_commands.TryDequeue(out var command)) command();
                if (!Hosts.Any(host => host.Active) ||
                    cadence.ShouldSubmit(Stopwatch.GetTimestamp(), Stopwatch.Frequency, 60))
                    for (var i = Hosts.Count - 1; i >= 0; i--)
                        if (!Hosts[i].Pump()) Hosts.RemoveAt(i);
                if (Hosts.Any(host => host.Active))
                    timer.Wait(cadence.Delay100ns(Stopwatch.GetTimestamp(), Stopwatch.Frequency));
                else
                    _wake.WaitOne(); // No active views: no polling or GPU work.
            }
        }
    }
}
