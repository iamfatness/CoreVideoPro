using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// DXGI shared-handle rules for distinguishing stub sentinel values from presentable GPU surfaces.
/// </summary>
public static class SharedTextureInteropRules
{
    /// <summary>
    /// Stub modules emit this sentinel until real D3D11 shared-texture export is enabled.
    /// </summary>
    public const ulong StubSentinelHandle = 0xFEEDFACEul;

    public static bool IsStubHandle(ulong ntHandle) => ntHandle == StubSentinelHandle;

    public static bool IsPresentableHandle(ulong ntHandle) =>
        ntHandle != 0 && !IsStubHandle(ntHandle);

    public static bool IsPresentable(ProgramSharedTexture texture) =>
        texture.IsValid &&
        IsPresentableHandle(CoreProtocolParser.ParseSharedHandleHex(texture.SharedHandleHex));

    public static bool ShouldPreferGpuSurface(ProgramSharedTexture? texture) =>
        texture is not null && IsPresentable(texture);
}