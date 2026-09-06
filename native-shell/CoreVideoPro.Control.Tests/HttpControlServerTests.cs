using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using CoreVideoPro.Control.Http;
using Xunit;

namespace CoreVideoPro.Control.Tests;

public sealed class HttpControlServerTests
{
    private static int GetFreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    [Fact]
    public async Task Http_GetManifest_And_PostInvoke_WorkOverTheWire()
    {
        var surface = new FakeControlSurface();
        var port = GetFreePort();
        await using var server = new HttpControlServer(surface, new HttpControlServerOptions { ListenPort = port });
        server.Start();

        using var http = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}/") };

        var manifest = await http.GetStringAsync("manifest");
        Assert.Contains("transport.take", manifest);

        var invoke = await http.PostAsync("invoke",
            new StringContent("{\"action\":\"transport.record\\u002Eset\",\"args\":[true]}", Encoding.UTF8, "application/json"));
        Assert.Equal(HttpStatusCode.OK, invoke.StatusCode);

        var (actionId, args) = Assert.Single(surface.Invocations);
        Assert.Equal("transport.record.set", actionId);
        Assert.True((bool)args[0]!);
    }

    [Fact]
    public async Task WebSocket_ReceivesInitialStateAndPushesUpdates()
    {
        var surface = new FakeControlSurface { State = ControlState.Empty with { Recording = false } };
        var port = GetFreePort();
        await using var server = new HttpControlServer(surface, new HttpControlServerOptions { ListenPort = port });
        server.Start();

        using var ws = new ClientWebSocket();
        await ws.ConnectAsync(new Uri($"ws://127.0.0.1:{port}/ws"), CancellationToken.None);

        // Initial snapshot on connect.
        var initial = await ReceiveJsonAsync(ws);
        Assert.False(initial.GetProperty("recording").GetBoolean());

        // A state change is pushed.
        surface.RaiseStateChanged(ControlState.Empty with { Recording = true, ActiveSceneId = "live" });
        var update = await ReceiveJsonAsync(ws);
        Assert.True(update.GetProperty("recording").GetBoolean());
        Assert.Equal("live", update.GetProperty("activeSceneId").GetString());

        await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, null, CancellationToken.None);
    }

    [Fact]
    public async Task AuthToken_RejectsMissingBearer()
    {
        var surface = new FakeControlSurface();
        var port = GetFreePort();
        await using var server = new HttpControlServer(surface, new HttpControlServerOptions { ListenPort = port, AuthToken = "s3cret" });
        server.Start();

        using var http = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}/") };

        var unauthorized = await http.GetAsync("manifest");
        Assert.Equal(HttpStatusCode.Unauthorized, unauthorized.StatusCode);

        http.DefaultRequestHeaders.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", "s3cret");
        var ok = await http.GetAsync("manifest");
        Assert.Equal(HttpStatusCode.OK, ok.StatusCode);
    }

    [Theory]
    [InlineData("+", null)]
    [InlineData("*", "")]
    [InlineData("0.0.0.0", "   ")]
    [InlineData("[::]", null)]
    [InlineData("192.168.1.20", null)]
    [InlineData("studio.local", null)]
    public async Task NetworkBindingWithoutTokenFailsBeforeListening(string host, string? token)
    {
        await using var server = new HttpControlServer(new FakeControlSurface(),
            new HttpControlServerOptions { Host = host, AuthToken = token, ListenPort = GetFreePort() });
        var error = Assert.Throws<InvalidOperationException>(() => server.Start());
        Assert.Contains("COREVIDEO_CONTROL_TOKEN", error.Message);
        // Failure must not leave a listener assigned and make the next start silently succeed.
        Assert.Throws<InvalidOperationException>(() => server.Start());
    }

    [Theory]
    [InlineData("127.0.0.1", null)]
    [InlineData("127.0.0.2", "")]
    [InlineData("localhost", null)]
    [InlineData("LOCALHOST", null)]
    [InlineData("[::1]", null)]
    [InlineData("+", "secret")]
    [InlineData("192.168.1.20", "secret")]
    public void ValidBindingPolicyDoesNotDependOnNetworkOrUrlAcl(string host, string? token)
    {
        new HttpControlServerOptions { Host = host, AuthToken = token }.Validate();
    }

    [Theory]
    [InlineData(null, "invoke")]
    [InlineData("wrong", "invoke")]
    [InlineData(null, "invoke?token=s3cret")]
    [InlineData("wrong", "invoke?token=s3cret")]
    public async Task UnauthorizedPostNeverInvokesAction(string? bearer, string path)
    {
        var surface = new FakeControlSurface();
        var port = GetFreePort();
        await using var server = new HttpControlServer(surface,
            new HttpControlServerOptions { ListenPort = port, AuthToken = "s3cret" });
        server.Start();
        using var http = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}/") };
        if (bearer is not null)
            http.DefaultRequestHeaders.Authorization = new("Bearer", bearer);
        using var response = await http.PostAsync(path,
            new StringContent("{\"action\":\"transport.take\",\"args\":[]}", Encoding.UTF8, "application/json"));
        Assert.Equal(HttpStatusCode.Unauthorized, response.StatusCode);
        Assert.Empty(surface.Invocations);

        http.DefaultRequestHeaders.Authorization = new("bearer", "s3cret");
        using var authorized = await http.PostAsync("invoke",
            new StringContent("{\"action\":\"transport.take\",\"args\":[]}", Encoding.UTF8, "application/json"));
        Assert.Equal(HttpStatusCode.OK, authorized.StatusCode);
        Assert.Single(surface.Invocations);
    }

    [Theory]
    [InlineData(null, null, false)]
    [InlineData("wrong", null, false)]
    [InlineData(null, "wrong", false)]
    [InlineData("s3cret", null, true)]
    [InlineData(null, "s3cret", true)]
    public async Task WebSocketAuthenticatesBeforeSendingState(string? bearer, string? query, bool allowed)
    {
        var surface = new FakeControlSurface();
        var port = GetFreePort();
        await using var server = new HttpControlServer(surface,
            new HttpControlServerOptions { ListenPort = port, AuthToken = "s3cret" });
        server.Start();
        using var ws = new ClientWebSocket();
        if (bearer is not null)
            ws.Options.SetRequestHeader("Authorization", $"Bearer {bearer}");
        var uri = new Uri($"ws://127.0.0.1:{port}/ws" + (query is null ? "" : $"?token={query}"));
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        if (allowed)
        {
            await ws.ConnectAsync(uri, timeout.Token);
            var initial = await ReceiveJsonAsync(ws);
            Assert.False(initial.GetProperty("recording").GetBoolean());
            await ws.CloseAsync(WebSocketCloseStatus.NormalClosure, null, timeout.Token);
        }
        else
        {
            var error = await Assert.ThrowsAsync<WebSocketException>(() => ws.ConnectAsync(uri, timeout.Token));
            Assert.Contains("401", error.Message);
        }
        Assert.Empty(surface.Invocations);
    }

    private static async Task<JsonElement> ReceiveJsonAsync(ClientWebSocket ws)
    {
        var buffer = new byte[8192];
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(3));
        var sb = new StringBuilder();
        WebSocketReceiveResult result;
        do
        {
            result = await ws.ReceiveAsync(new ArraySegment<byte>(buffer), cts.Token);
            sb.Append(Encoding.UTF8.GetString(buffer, 0, result.Count));
        }
        while (!result.EndOfMessage);

        return JsonDocument.Parse(sb.ToString()).RootElement.Clone();
    }
}
