using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class ControlObservationDemandTests
{
    [Fact]
    public async Task SnapshotProjectionIsDeferredSharedAndStillServedByHttp()
    {
        var reads = 0;
        var observation = JsonSerializer.SerializeToElement(new { worker = "actual", frames = 42 });
        var state = ControlState.Empty.WithNativeObservation(() =>
        {
            Interlocked.Increment(ref reads);
            return observation;
        });
        var changed = state with { Recording = true };
        Assert.True(changed.Recording);
        Assert.Equal(0, reads);
        var response = await new HttpControlRouter(new FakeControlSurface { State = changed })
            .HandleAsync("GET", "/state", null);
        using var body = JsonDocument.Parse(response.Body);
        Assert.Equal(42, body.RootElement.GetProperty("nativeObservation").GetProperty("frames").GetInt32());
        Parallel.For(0, 50, _ => Assert.Equal("actual", state.NativeObservation!.Value.GetProperty("worker").GetString()));
        Assert.Equal(1, reads);
        Assert.Null((state with { NativeObservation = null }).NativeObservation);
    }
}
