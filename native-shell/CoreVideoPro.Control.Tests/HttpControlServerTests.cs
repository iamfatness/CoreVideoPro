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
