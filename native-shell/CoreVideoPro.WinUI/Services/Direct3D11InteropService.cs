using System.Runtime.InteropServices;
using CoreVideoPro.WinUI.Models;
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
    [ComImport]
    [Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IDirect3DDxgiInterfaceAccess
    {
        IntPtr GetInterface(ref Guid iid);
    }

    [ComImport]
    [Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ISwapChainPanelNative
    {
        void SetSwapChain(IntPtr swapChain);
    }

    private static readonly Guid D3D11DeviceGuid = new("DDB0B8B9-9585-4E8C-9CA0-8776E7C35E96");

    private IDirect3DDevice? _winrtDevice;
    private ID3D11Device? _device;
    private ID3D11DeviceContext? _context;
    private IDXGISwapChain1? _swapChain;
    private ID3D11Texture2D? _backBuffer;
    private SwapChainPanel? _panel;
    private int _surfaceWidth;
    private int _surfaceHeight;
    private bool _disposed;

    public bool IsReady => _device is not null && _swapChain is not null && _backBuffer is not null;

    public nint DevicePointer
    {
        get
        {
            if (_winrtDevice is null)
            {
                return 0;
            }

            var unknown = Marshal.GetIUnknownForObject(_winrtDevice);
            Marshal.Release(unknown);
            return unknown;
        }
    }

    public bool TryAttachSwapChainPanel(SwapChainPanel panel)
    {
        if (_disposed)
        {
            return false;
        }

        _panel = panel;
        return EnsureDevice() && EnsureSwapChain();
    }

    public bool TryPresentSharedTexture(SharedTextureHandle handle)
    {
        if (_disposed || !handle.IsValid || !EnsureDevice() || !EnsureSwapChain(handle.Width, handle.Height))
        {
            return false;
        }

        try
        {
            using var sharedTexture = _device!.OpenSharedResource<ID3D11Texture2D>((IntPtr)handle.NtHandle);
            _context!.CopyResource(_backBuffer!, sharedTexture);
            _swapChain!.Present(1, PresentFlags.None);
            return true;
        }
        catch
        {
            return false;
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _backBuffer?.Dispose();
        _swapChain?.Dispose();
        _context?.Dispose();
        _device?.Dispose();
        _winrtDevice?.Dispose();
        _backBuffer = null;
        _swapChain = null;
        _context = null;
        _device = null;
        _winrtDevice = null;
        _panel = null;
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
                return true;
            }

            _winrtDevice = (IDirect3DDevice)Marshal.GetObjectForIUnknown(winrtDevicePtr);
            Marshal.Release(winrtDevicePtr);
            return true;
        }
        catch
        {
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

        if (_swapChain is not null && _surfaceWidth == targetWidth && _surfaceHeight == targetHeight)
        {
            return true;
        }

        _backBuffer?.Dispose();
        _swapChain?.Dispose();
        _backBuffer = null;
        _swapChain = null;

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
        var panelNative = (ISwapChainPanelNative)Marshal.GetObjectForIUnknown(Marshal.GetIUnknownForObject(_panel));
        panelNative.SetSwapChain(_swapChain.NativePointer);

        _backBuffer = _swapChain.GetBuffer<ID3D11Texture2D>(0);
        _surfaceWidth = targetWidth;
        _surfaceHeight = targetHeight;
        return true;
    }

    [DllImport("d3d11.dll", ExactSpelling = true, PreserveSig = true)]
    private static extern int CreateDirect3D11DeviceFromDXGIDevice(IntPtr dxgiDevice, out IntPtr graphicsDevice);
}