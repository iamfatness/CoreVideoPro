using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Leased GPU configuration and bounded native observations. Presentation
/// never calls back into the CLR, XAML or the managed control worker.</summary>
internal sealed class NativeMonitorPresentation : IDisposable
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct Stats
    {
        public ulong Handle, Attempts, RetryAttempts, Presents, Ingested, AcquireAttempts, AcquireBusy, NotReady, PresentBusy;
        public long LastCommitQpc, MaximumGapQpc, ObservedQpc;
        public double IngestMs, CopyMs, CopyMaximumMs, PresentMs, PresentMaximumMs, TotalMs;
        public int HResult;
        public uint Active, Pixel, PixelValid;
    }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint SizeFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate nint CreateFn(nint device, nint context, nint chain, nint backBuffer, ulong handle, nint ready, uint flags);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ReleaseFn(nint lease);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void ActiveFn(OwnerHandle lease, int active);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void CommandFn(OwnerHandle lease);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int SnapshotFn(OwnerHandle lease, out Stats result);
    private sealed record Api(CreateFn Create, ReleaseFn Release, ActiveFn Active, CommandFn Reset, CommandFn Pixel, SnapshotFn Snapshot);
    private static readonly Lazy<Api?> Library = new(Load);
    internal static bool Available => Library.Value is not null;
    private sealed class OwnerHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        internal OwnerHandle(nint value) : base(true) => SetHandle(value);
        protected override bool ReleaseHandle() { Library.Value!.Release(handle); return true; }
    }
    private readonly OwnerHandle _owner;
    internal ulong SharedHandle { get; }
    internal long ChainRevision { get; }
    internal long CountOffset { get; }
    internal NativeMonitorPresentation(nint device, nint context, nint chain, nint backBuffer,
        ulong handle, nint ready, long revision, long countOffset)
    {
        var value = Library.Value!.Create(device, context, chain, backBuffer, handle, ready, 0);
        if (value == 0) throw new InvalidOperationException("Native monitor owner refused its resource lease.");
        _owner = new(value); SharedHandle = handle; ChainRevision = revision; CountOffset = countOffset;
    }
    internal bool TrySnapshot(out Stats result)
    {
        result = default;
        if (_owner.IsClosed) return false;
        try { return Library.Value!.Snapshot(_owner, out result) != 0; }
        catch (ObjectDisposedException) { return false; }
    }
    internal void SetActive(bool active)
    {
        if (_owner.IsClosed) return;
        try { Library.Value!.Active(_owner, active ? 1 : 0); } catch (ObjectDisposedException) { }
    }
    internal void ResetProbe()
    {
        if (_owner.IsClosed) return;
        try { Library.Value!.Reset(_owner); } catch (ObjectDisposedException) { }
    }
    internal uint? ReadPixel()
    {
        if (_owner.IsClosed) return null;
        try
        {
            Library.Value!.Pixel(_owner);
            return TrySnapshot(out var stats) && stats.Presents > 0 && stats.PixelValid != 0 ? stats.Pixel : null;
        }
        catch (ObjectDisposedException) { return null; }
    }
    public void Dispose() => _owner.Dispose(); // Release enqueues native retirement; no UI join.
    private static Api? Load()
    {
        var path = Environment.GetEnvironmentVariable("COREVIDEO_NATIVE_MONITOR_DLL_PATH");
        path = string.IsNullOrWhiteSpace(path) ? Path.Combine(AppContext.BaseDirectory, "corevideo-monitor-presenter.dll") : Path.GetFullPath(path);
        if (!File.Exists(path)) { LaunchLog.Write("monitor-native: DLL unavailable; managed fallback remains unqualified"); return null; }
        try
        {
            // Held for process lifetime: native owners can outlive managed view retirement.
            var module = NativeLibrary.Load(path);
            T Export<T>(string name) where T : Delegate => Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(module, name));
            if (Export<SizeFn>("cv_monitor_stats_size")() != Marshal.SizeOf<Stats>()) throw new InvalidOperationException("Native monitor observation ABI mismatch.");
            LaunchLog.Write("monitor-native: independent presentation owner loaded");
            return new(Export<CreateFn>("cv_monitor_create"), Export<ReleaseFn>("cv_monitor_release"),
                Export<ActiveFn>("cv_monitor_active"), Export<CommandFn>("cv_monitor_probe_reset"),
                Export<CommandFn>("cv_monitor_probe_pixel"), Export<SnapshotFn>("cv_monitor_snapshot"));
        }
        catch (Exception error)
        {
            LaunchLog.Write("monitor-native: unavailable: " + error.Message);
            return null;
        }
    }
}
