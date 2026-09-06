using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class HttpControlRouterTests
{
    [Fact]
    public async Task NativeObservationFieldsAreAdditiveInHttpState()
    {
        var surface = new FakeControlSurface { State = new ControlState
        {
            ActiveSceneId = "desired", NativeActiveSceneId = "native",
            NativePreviewSceneId = "queued", NativeLowerThirdPhase = "building-in",
            NativeLowerThirdVisible = true, NativeProgramFrameCount = 80
        }};
        var response = await new HttpControlRouter(surface).HandleAsync("GET", "/state", null);
        using var doc = JsonDocument.Parse(response.Body);
        Assert.Equal("desired", doc.RootElement.GetProperty("activeSceneId").GetString());
        Assert.Equal("native", doc.RootElement.GetProperty("nativeActiveSceneId").GetString());
        Assert.Equal("queued", doc.RootElement.GetProperty("nativePreviewSceneId").GetString());
        Assert.Equal("building-in", doc.RootElement.GetProperty("nativeLowerThirdPhase").GetString());
        Assert.True(doc.RootElement.GetProperty("nativeLowerThirdVisible").GetBoolean());
        Assert.Equal(80, doc.RootElement.GetProperty("nativeProgramFrameCount").GetInt32());
    }

    [Fact]
    public async Task Get_Manifest_ReturnsTheActionContract()
    {
        var router = new HttpControlRouter(new FakeControlSurface());
        var response = await router.HandleAsync("GET", "/manifest", null);

        Assert.Equal(200, response.Status);
        Assert.Contains("transport.take", response.Body);
        Assert.Contains("automation.magic", response.Body);
        Assert.Contains("/cvp/transport/take", response.Body);
    }

    [Fact]
    public async Task Get_State_ReturnsCamelCaseState()
    {
        var surface = new FakeControlSurface
        {
            State = ControlState.Empty with
            {
                Recording = true,
                ActiveSceneId = "interview",
                ZoomAudioMode = "perGuestIso",
                ProgramTruePeakDbfs = -8.5,
                AudioSources = [new ControlAudioSourceState("zoom:p-1", "Host", 82, -6.2, -18.4, false, "native-pcm")]
            }
        };
        var router = new HttpControlRouter(surface);

        var response = await router.HandleAsync("GET", "/state", null);

        Assert.Equal(200, response.Status);
        using var doc = JsonDocument.Parse(response.Body);
        Assert.True(doc.RootElement.GetProperty("recording").GetBoolean());
        Assert.Equal("interview", doc.RootElement.GetProperty("activeSceneId").GetString());
        Assert.Equal("perGuestIso", doc.RootElement.GetProperty("zoomAudioMode").GetString());
        Assert.Equal(-8.5, doc.RootElement.GetProperty("programTruePeakDbfs").GetDouble());
        var source = Assert.Single(doc.RootElement.GetProperty("audioSources").EnumerateArray());
        Assert.Equal("zoom:p-1", source.GetProperty("sourceId").GetString());
        Assert.Equal(-6.2, source.GetProperty("peakDbfs").GetDouble());
    }

    [Fact]
    public async Task Post_Invoke_BindsArgsAndInvokesTheSurface()
    {
        var surface = new FakeControlSurface();
        var router = new HttpControlRouter(surface);

        var response = await router.HandleAsync("POST", "/invoke", "{\"action\":\"input.assign\",\"args\":[3,\"zoom:p-1\"]}");

        Assert.Equal(200, response.Status);
        Assert.Contains("\"ok\":true", response.Body);
        var (actionId, args) = Assert.Single(surface.Invocations);
        Assert.Equal("input.assign", actionId);
        Assert.Equal(3, args[0]);
        Assert.Equal("zoom:p-1", args[1]);
    }

    [Fact]
    public async Task Post_Invoke_RejectsUnknownAndMalformed()
    {
        var surface = new FakeControlSurface();
        var router = new HttpControlRouter(surface);

        Assert.Equal(400, (await router.HandleAsync("POST", "/invoke", "{\"action\":\"bogus\"}")).Status);
        Assert.Equal(400, (await router.HandleAsync("POST", "/invoke", "not json")).Status);
        Assert.Equal(400, (await router.HandleAsync("POST", "/invoke", "{\"args\":[1]}")).Status);   // missing action
        Assert.Equal(400, (await router.HandleAsync("POST", "/invoke", "{\"action\":\"scene.select\"}")).Status);  // missing required arg
        Assert.Empty(surface.Invocations);
    }

    [Fact]
    public async Task Post_Invoke_Returns422WhenSurfaceFails()
    {
        var surface = new FakeControlSurface { Handler = (_, _) => ControlInvokeResult.Fail("slot out of range") };
        var router = new HttpControlRouter(surface);

        var response = await router.HandleAsync("POST", "/invoke", "{\"action\":\"input.name\",\"args\":[99,\"x\"]}");
        Assert.Equal(422, response.Status);
        Assert.Contains("slot out of range", response.Body);
    }

    [Fact]
    public async Task UnknownRoute_Returns404()
    {
        var router = new HttpControlRouter(new FakeControlSurface());
        Assert.Equal(404, (await router.HandleAsync("GET", "/nope", null)).Status);
    }
}
