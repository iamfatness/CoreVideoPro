using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class SharedTextureInteropRulesTests
{
    [Theory]
    [InlineData(0xFEEDFACEul, true)]
    [InlineData(0xABCDul, false)]
    [InlineData(0ul, false)]
    public void IsStubHandle_RecognizesSentinel(ulong handle, bool expected)
    {
        Assert.Equal(expected, SharedTextureInteropRules.IsStubHandle(handle));
    }

    [Theory]
    [InlineData(0xFEEDFACEul, false)]
    [InlineData(0x00000000DEADBEEFul, true)]
    [InlineData(0x10ul, true)]
    [InlineData(0ul, false)]
    public void IsPresentableHandle_RejectsStubAndZero(ulong handle, bool expected)
    {
        Assert.Equal(expected, SharedTextureInteropRules.IsPresentableHandle(handle));
    }

    [Fact]
    public void StubWireTextureIsParsedButNotPresentable()
    {
        var texture = new ProgramSharedTexture
        {
            SharedHandleHex = "0xFEEDFACE",
            Width = 1920,
            Height = 1080,
            Format = "B8G8R8A8_UNORM",
            FrameNumber = 4
        };

        Assert.True(texture.IsValid);
        Assert.False(SharedTextureInteropRules.IsPresentable(texture));
        Assert.False(SharedTextureInteropRules.ShouldPreferGpuSurface(texture));
    }

    [Fact]
    public void RealHandleTextureIsPresentable()
    {
        var texture = new ProgramSharedTexture
        {
            SharedHandleHex = "0x00000000DEADBEEF",
            Width = 1920,
            Height = 1080,
            Format = "B8G8R8A8_UNORM",
            FrameNumber = 8
        };

        Assert.True(SharedTextureInteropRules.IsPresentable(texture));
        Assert.True(SharedTextureInteropRules.ShouldPreferGpuSurface(texture));
    }

    [Fact]
    public void PreviewWithStubHandleAndBgraFallsBackToCpuPath()
    {
        var pixels = new byte[4];
        var preview = new ProgramFramePreview
        {
            FrameNumber = 2,
            Width = 1,
            Height = 1,
            Renderer = "d3d11",
            Health = "live",
            Bgra = pixels,
            SharedTexture = new ProgramSharedTexture
            {
                SharedHandleHex = "0xFEEDFACE",
                Width = 1920,
                Height = 1080
            }
        };

        Assert.False(SharedTextureInteropRules.ShouldPreferGpuSurface(preview.SharedTexture));
        Assert.True(preview.Bgra.Length > 0);
    }

    [Fact]
    public void PreviewWithRealHandlePrefersGpuSurface()
    {
        var preview = new ProgramFramePreview
        {
            FrameNumber = 2,
            Width = 1920,
            Height = 1080,
            Renderer = "d3d11",
            Health = "live",
            Bgra = [1, 2, 3, 255],
            SharedTexture = new ProgramSharedTexture
            {
                SharedHandleHex = "0x0000000042AABBCC",
                Width = 1920,
                Height = 1080
            }
        };

        Assert.True(SharedTextureInteropRules.ShouldPreferGpuSurface(preview.SharedTexture));
    }
}