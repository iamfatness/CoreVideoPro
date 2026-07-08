using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

// virtual-camera-spec V2/V4: the shell read model for the core's virtualCamera
// snapshot block deserializes via the shared camelCase options.
public sealed class VirtualCameraReadModelTests
{
    private static readonly JsonSerializerOptions CamelCase = new(JsonSerializerDefaults.Web);

    [Fact]
    public void DeserializesVirtualCameraBlock()
    {
        const string json = """
            {
              "enabled": true,
              "status": "live",
              "deviceName": "CoreVideo Pro Camera",
              "resolution": { "width": 1280, "height": 720 },
              "fps": 30,
              "framesPublished": 1500,
              "warning": ""
            }
            """;

        var vc = JsonSerializer.Deserialize<NativeMediaCoreVirtualCamera>(json, CamelCase);

        Assert.NotNull(vc);
        Assert.True(vc!.Enabled);
        Assert.Equal("live", vc.Status);
        Assert.Equal("CoreVideo Pro Camera", vc.DeviceName);
        Assert.Equal(1280, vc.Resolution.Width);
        Assert.Equal(720, vc.Resolution.Height);
        Assert.Equal(30, vc.Fps);
        Assert.Equal(1500L, vc.FramesPublished);
        Assert.Equal(string.Empty, vc.Warning);
    }

    [Fact]
    public void DefaultsAreSafeWhenBlockAbsent()
    {
        // A snapshot with no virtualCamera block yields the default (off) model.
        var vc = new NativeMediaCoreVirtualCamera();
        Assert.False(vc.Enabled);
        Assert.Equal("off", vc.Status);
        Assert.Equal(0, vc.Resolution.Width);
    }

    [Fact]
    public void DeserializesFailedStateWithWarning()
    {
        const string json = """
            { "enabled": false, "status": "failed", "warning": "Needs Windows 11 22000+" }
            """;
        var vc = JsonSerializer.Deserialize<NativeMediaCoreVirtualCamera>(json, CamelCase);
        Assert.NotNull(vc);
        Assert.Equal("failed", vc!.Status);
        Assert.Equal("Needs Windows 11 22000+", vc.Warning);
    }
}
