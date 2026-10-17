using CoreVideoPro.WinUI.Services;
using Windows.UI;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;
public sealed class GraphicsColorTests
{
    [Fact] public void GraphicsRgbaPreservesAlphaWithoutChangingLegacyStyleColourContract()
    {
        var rgba=GraphicsColor.Parse("#12345678",default);Assert.Equal((byte)0x78,rgba.A);Assert.Equal((byte)0x12,rgba.R);Assert.Equal((byte)0x34,rgba.G);Assert.Equal((byte)0x56,rgba.B);
        Assert.Equal((byte)255,GraphicsColor.Parse("#123456",default).A);Assert.False(HexColor.TryParse("#12345678",out _));
        var fallback=Color.FromArgb(1,2,3,4);Assert.Equal(fallback,GraphicsColor.Parse("#GGGGGG",fallback));
    }
}
