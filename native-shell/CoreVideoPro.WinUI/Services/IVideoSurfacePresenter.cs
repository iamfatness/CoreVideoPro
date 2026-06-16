using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Presentation seam for a single WinUI video tile. Metadata is live today;
/// shared-texture presentation is stubbed for future SwapChainPanel wiring.
/// </summary>
public interface IVideoSurfacePresenter
{
    string SurfaceKey { get; }

    void UpdateMetadata(VideoFrameMetadata metadata, string statusLine, string? detailLine = null);

    /// <summary>
    /// Accept a DXGI shared handle from the native compositor. Returns false when
    /// IDirect3DDevice interop is not yet initialized.
    /// </summary>
    bool TryAcceptSharedTextureHandle(SharedTextureHandle handle);

    void Clear();
}

/// <summary>
/// Future WinUI ↔ native D3D interop extension point (IDirect3DDevice / SwapChainPanel).
/// </summary>
public interface IDirect3DVideoSurfaceHost
{
    bool IsDirect3DReady { get; }

    /// <param name="devicePointer">Opaque IDirect3DDevice pointer from WinRT interop.</param>
    void SetDirect3DDevice(nint devicePointer);

    void PresentSharedHandle(SharedTextureHandle handle);
}