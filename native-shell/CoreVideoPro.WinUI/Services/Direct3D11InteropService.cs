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
            using var sharedTexture = _device!.OpenSharedResource<ID3D11Texture2D>((IntPtr)handle.NtHandle);
            _context!.CopyResource(_backBuffer!, sharedTexture);
            _swapChain!.Present(1, PresentFlags.None);
            _lastPresentedHandle = handle.NtHandle;
            SetPresentationPath(PresentationPath.GpuActive);
            LaunchLog.Write($"d3d: presented shared handle 0x{handle.NtHandle:X} {handle.Width}x{handle.Height}");
            return true;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"d3d: present FAILED 0x{handle.NtHandle:X} {handle.Width}x{handle.Height}: {ex.GetType().Name}: {ex.Message}");
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
        _context?.Dispose();
        _device?.Dispose();
        _winrtDevice?.Dispose();
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
            return;
        }

        EnsureSwapChain(width, height);
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
        _backBuffer?.Dispose();
        _swapChain?.Dispose();
        _backBuffer = null;
        _swapChain = null;
        _surfaceWidth = 0;
        _surfaceHeight = 0;
    }

    private void ResetDevice()
    {
        ResetSwapChain();
        _context?.Dispose();
        _device?.Dispose();
        _winrtDevice?.Dispose();
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