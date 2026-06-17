using System.Runtime.InteropServices;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// WinUI 3 <see cref="SwapChainPanel"/> exposes <c>ISwapChainPanelNative</c> via COM, not WinRT projection.
/// IID from microsoft.ui.xaml.media.dxinterop.idl (Windows App SDK 2.x).
/// </summary>
internal static class SwapChainPanelNativeInterop
{
    private static readonly Guid SwapChainPanelNativeIid = new("63aad0b8-7c24-40ff-85a8-640d944cc325");
    private static readonly Guid SwapChainPanelNative2Iid = new("88fd8248-10da-4810-bb4c-010dd27faea9");

    internal static bool TrySetSwapChain(SwapChainPanel panel, IntPtr swapChainPointer, out string? failureReason)
    {
        failureReason = null;
        if (swapChainPointer == IntPtr.Zero)
        {
            failureReason = "swapChainPointer is zero";
            return false;
        }

        var panelUnknown = Marshal.GetIUnknownForObject(panel);
        try
        {
            if (TryQuery(panelUnknown, SwapChainPanelNativeIid, out var nativePtr))
            {
                try
                {
                    var native = (ISwapChainPanelNative)Marshal.GetObjectForIUnknown(nativePtr);
                    var hr = native.SetSwapChain(swapChainPointer);
                    if (hr >= 0)
                    {
                        return true;
                    }

                    failureReason = $"ISwapChainPanelNative.SetSwapChain hr=0x{hr:X8}";
                }
                finally
                {
                    Marshal.Release(nativePtr);
                }
            }

            if (TryQuery(panelUnknown, SwapChainPanelNative2Iid, out var native2Ptr))
            {
                try
                {
                    var native2 = (ISwapChainPanelNative2)Marshal.GetObjectForIUnknown(native2Ptr);
                    var hr = native2.SetSwapChain(swapChainPointer);
                    if (hr >= 0)
                    {
                        return true;
                    }

                    failureReason = $"ISwapChainPanelNative2.SetSwapChain hr=0x{hr:X8}";
                }
                finally
                {
                    Marshal.Release(native2Ptr);
                }
            }

            failureReason ??= "SwapChainPanel does not implement ISwapChainPanelNative";
            return false;
        }
        finally
        {
            Marshal.Release(panelUnknown);
        }
    }

    private static bool TryQuery(IntPtr unknown, Guid iid, out IntPtr interfacePtr)
    {
        interfacePtr = IntPtr.Zero;
        var hr = Marshal.QueryInterface(unknown, ref iid, out interfacePtr);
        return hr >= 0 && interfacePtr != IntPtr.Zero;
    }

    [ComImport]
    [Guid("63aad0b8-7c24-40ff-85a8-640d944cc325")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ISwapChainPanelNative
    {
        [PreserveSig]
        int SetSwapChain(IntPtr swapChain);
    }

    [ComImport]
    [Guid("88fd8248-10da-4810-bb4c-010dd27faea9")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ISwapChainPanelNative2
    {
        [PreserveSig]
        int SetSwapChain(IntPtr swapChain);
    }
}