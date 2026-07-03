using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class HttpControlRouterTests
{
    [Fact]
    public async Task Get_Manifest_ReturnsTheActionContract()
    {
        var router = new HttpControlRouter(new FakeControlSurface());
        var response = await router.HandleAsync("GET", "/manifest", null);

        Assert.Equal(200, response.Status);
        Assert.Contains("transport.take", response.Body);
        Assert.Contains("/cvp/transport/take", response.Body);
    }

    [Fact]
    public async Task Get_State_ReturnsCamelCaseState()
    {
        var surface = new FakeControlSurface { State = ControlState.Empty with { Recording = true, ActiveSceneId = "interview" } };
        var router = new HttpControlRouter(surface);

        var response = await router.HandleAsync("GET", "/state", null);

        Assert.Equal(200, response.Status);
        using var doc = JsonDocument.Parse(response.Body);
        Assert.True(doc.RootElement.GetProperty("recording").GetBoolean());
        Assert.Equal("interview", doc.RootElement.GetProperty("activeSceneId").GetString());
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
