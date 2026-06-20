using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class SrtIngestSourceTests
{
    [Fact]
    public void SourceBuildsStableDeviceIdentityAndUri()
    {
        var source = new SrtIngestSource
        {
            Id = "srt-source-03",
            Number = 3,
            Host = "192.168.1.20",
            Port = "10002",
            LatencyMs = "200"
        };

        Assert.Equal("srt-ingest-03", source.DeviceId);
        Assert.Equal("SRT 3", source.Name);
        Assert.Equal("srt://192.168.1.20:10002", source.NativeUri);
        Assert.Equal("listener 192.168.1.20:10002 - 200 ms latency", source.Summary);
    }
}
