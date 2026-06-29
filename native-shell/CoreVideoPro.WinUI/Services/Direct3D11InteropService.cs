using System;
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

    private readonly HashSet<ulong> _invalidHandles = [];
    private IDirect3DDevice? _winrtDevice;
    private ID3D11Device? _device;
    private ID3D11DeviceContext? _context;
    private IDXGISwapChain1? _swapChain;
    private ID3D11Texture2D? _backBuffer;
    private SwapChainPanel? _panel;
    private int _surfaceWidth;
    private int _surfaceHeight;
    private int _panelWidth;
    private int _panelHeight;
    private ulong _lastPresentedHandle;
    private long _presentCount;
    private ID3D11Texture2D? _cachedSharedTexture;
    private IDXGIKeyedMutex? _cachedKeyedMutex;
    private KeyedMutexAcquireSyncThunk? _cachedAcquireSync;
    private ulong _cachedSharedHandle;

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
    private nint _cachedDevicePointer;
    private bool _disposed;
    private PresentationPath _path = PresentationPath.Uninitialized;

    public event Action? PresentationPathChanged;

    public bool IsReady => _device is not null && _swapChain is not null && _backBuffer is not null;

    public bool IsGpuPresenting => _path == PresentationPath.GpuActive;

    public bool IsCpuFallback => _path == PresentationPath.CpuFallback;

    public PresentationPath ActivePath => _path;

    public nint DevicePointer => _cachedDevicePointer;

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

        if (!EnsureDevice())
        {
            LaunchLog.Write("d3d: present skip — EnsureDevice failed");
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }

        if (!EnsureSwapChain(handle.Width, handle.Height))
        {
            LaunchLog.Write($"d3d: present skip — EnsureSwapChain failed {handle.Width}x{handle.Height}");
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }

        try
        {
            // Open the shared texture once per handle and cache it. The compositor
            // reuses the same shared texture, and present now runs every vsync, so
            // re-opening it 60x/sec would churn (and leak) KMT shared handles and
            // crash after a few seconds. Re-open only when the handle value changes.
            if (_cachedSharedTexture is null || _cachedSharedHandle != handle.NtHandle)
            {
                _cachedKeyedMutex?.Dispose();
                _cachedSharedTexture?.Dispose();
                _cachedSharedTexture = _device!.OpenSharedResource<ID3D11Texture2D>((IntPtr)handle.NtHandle);
                _cachedKeyedMutex = _cachedSharedTexture.QueryInterface<IDXGIKeyedMutex>();
                _cachedAcquireSync = BuildAcquireSyncThunk(_cachedKeyedMutex);
                _cachedSharedHandle = handle.NtHandle;
            }
            // Consumer side of the keyed mutex: acquire key 1 (the core releases 1
            // after writing a new frame), non-blocking (0ms) so the UI thread never
            // stalls. Vortice's AcquireSync returns void and can't report
            // WAIT_TIMEOUT (a SUCCEEDED HRESULT), so call the COM vtable directly to
            // get the real HRESULT. hr != S_OK => no new frame: keep the last one.
            //
            // SKIP-PRESENT (deliberate): present ONLY on a new keyed-mutex frame. The
            // "smooth" variant that re-presents the last backbuffer every vsync on idle
            // destabilized the app (~31s fail-fast) — this method is shared by the
            // program monitor and now every multiview tile, so an idle re-present runs
            // N swap chains every vsync. Keep skip-present; it is the known-stable path.
            if (_cachedAcquireSync is not null && _cachedAcquireSync(_cachedKeyedMutex!.NativePointer, 1, 0) != 0)
            {
                // No new frame: leave the last presented backbuffer on screen.
                SetPresentationPath(PresentationPath.GpuActive);
                return true;
            }
            _context!.CopyResource(_backBuffer!, _cachedSharedTexture);
            _cachedKeyedMutex?.ReleaseSync(0);
            _swapChain!.Present(1, PresentFlags.None);
            _lastPresentedHandle = handle.NtHandle;
            SetPresentationPath(PresentationPath.GpuActive);
            // Present runs only on new frames; log a heartbeat every 120 presents.
            if (++_presentCount % 120 == 0)
            {
                LaunchLog.Write($"d3d: present #{_presentCount} 0x{handle.NtHandle:X} {handle.Width}x{handle.Height}");
            }
            return true;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"d3d: present FAILED 0x{handle.NtHandle:X} {handle.Width}x{handle.Height}: {ex.GetType().Name}: {ex.Message}");
            _cachedAcquireSync = null;
            _cachedKeyedMutex?.Dispose();
            _cachedKeyedMutex = null;
            _cachedSharedTexture?.Dispose();
            _cachedSharedTexture = null;
            _cachedSharedHandle = 0;
            InvalidateSharedHandle(handle.NtHandle);
            ResetSwapChain();
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }
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
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        DetachPanelHandlers();
        ResetSwapChain();
        // The WinRT IDirect3DDevice RCW throws InvalidCastException (E_NOINTERFACE on
        // the IDisposable QI) on teardown, which fail-fasts the app when a
        // VideoSurfaceHost unloads — e.g. leaving the meeting unloads the Zoom
        // participant tiles, and multiview tiles come and go as participants join/leave.
        // Teardown must never throw.
        try { _context?.Dispose(); } catch { }
        try { _device?.Dispose(); } catch { }
        try { _winrtDevice?.Dispose(); } catch { }
        _context = null;
        _device = null;
        _winrtDevice = null;
        _cachedDevicePointer = 0;
        _invalidHandles.Clear();
        _lastPresentedHandle = 0;
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
        if (_device is not null && _context is not null)
        {
            return true;
        }

        try
        {
            _device = D3D11.D3D11CreateDevice(
                DriverType.Hardware,
                DeviceCreationFlags.BgraSupport);
            _context = _device.ImmediateContext;

            using var dxgiDevice = _device.QueryInterface<IDXGIDevice>();
            var hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.NativePointer, out var winrtDevicePtr);
            if (hr < 0 || winrtDevicePtr == IntPtr.Zero)
            {
                SetPresentationPath(PresentationPath.DeviceReady);
                return true;
            }

            _winrtDevice = (IDirect3DDevice)Marshal.GetObjectForIUnknown(winrtDevicePtr);
            _cachedDevicePointer = winrtDevicePtr;
            Marshal.Release(winrtDevicePtr);
            SetPresentationPath(PresentationPath.DeviceReady);
            return true;
        }
        catch
        {
            ResetDevice();
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }
    }

    private bool EnsureSwapChain(int width = 0, int height = 0)
    {
        if (_panel is null || _device is null)
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

        try
        {
            ResetSwapChain();

            using var dxgiDevice = _device.QueryInterface<IDXGIDevice>();
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

            _swapChain = factory.CreateSwapChainForComposition(_device, swapChainDesc);
            if (!SwapChainPanelNativeInterop.TrySetSwapChain(_panel, _swapChain.NativePointer, out var attachFailure))
            {
                LaunchLog.Write($"d3d: panel attach failed: {attachFailure}");
                ResetSwapChain();
                SetPresentationPath(PresentationPath.CpuFallback);
                return false;
            }

            _backBuffer = _swapChain.GetBuffer<ID3D11Texture2D>(0);
            _surfaceWidth = targetWidth;
            _surfaceHeight = targetHeight;
            ApplyPanelTransform();
            return true;
        }
        catch
        {
            ResetSwapChain();
            SetPresentationPath(PresentationPath.CpuFallback);
            return false;
        }
    }

    private void ResetSwapChain()
    {
        // COM RCW disposes can throw E_NOINTERFACE during teardown — never let that
        // escape (it fail-fasts the app when a tile/host unloads).
        _cachedAcquireSync = null;
        try { _cachedKeyedMutex?.Dispose(); } catch { }
        _cachedKeyedMutex = null;
        try { _cachedSharedTexture?.Dispose(); } catch { }
        _cachedSharedTexture = null;
        _cachedSharedHandle = 0;
        try { _backBuffer?.Dispose(); } catch { }
        try { _swapChain?.Dispose(); } catch { }
        _backBuffer = null;
        _swapChain = null;
        _surfaceWidth = 0;
        _surfaceHeight = 0;
    }

    private void ResetDevice()
    {
        ResetSwapChain();
        try { _context?.Dispose(); } catch { }
        try { _device?.Dispose(); } catch { }
        try { _winrtDevice?.Dispose(); } catch { }
        _context = null;
        _device = null;
        _winrtDevice = null;
        _cachedDevicePointer = 0;
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