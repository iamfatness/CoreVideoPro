using System.Collections.Concurrent;
using System.Net;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.Control.Http;

public sealed record HttpControlServerOptions
{
    /// <summary>TCP port for the HTTP/WebSocket control API.</summary>
    public int ListenPort { get; init; } = 8011;

    /// <summary>Host for the HttpListener prefix. Loopback (default) needs no privileges; use "+"
    /// for LAN access (may require a urlacl / admin on Windows — the operator opts in).</summary>
    public string Host { get; init; } = "127.0.0.1";

    /// <summary>Bearer token, required for non-loopback hosts. When set, requests must send
    /// <c>Authorization: Bearer &lt;token&gt;</c> (or <c>?token=</c> for the WS upgrade).</summary>
    public string? AuthToken { get; init; }

    /// <summary>Validate policy before allocating or starting a listener. Hostnames other than
    /// localhost are treated as network bindings; DNS is not a security boundary.</summary>
    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Host))
            throw new ArgumentException("A control HTTP bind host is required.", nameof(Host));
        if (ListenPort is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(ListenPort));

        var loopback = string.Equals(Host, "localhost", StringComparison.OrdinalIgnoreCase) ||
            (IPAddress.TryParse(Host.Trim('[', ']'), out var address) && IPAddress.IsLoopback(address));
        if (!loopback && string.IsNullOrWhiteSpace(AuthToken))
            throw new InvalidOperationException("LAN HTTP/WS control requires a non-empty COREVIDEO_CONTROL_TOKEN. Set a token or disable COREVIDEO_HTTP_LAN to use loopback.");
    }
}

/// <summary>HTTP + WebSocket control transport over <see cref="HttpListener"/>. REST actions and
/// the manifest/state go through <see cref="HttpControlRouter"/>; <c>/ws</c> upgrades to a WebSocket
/// that receives the current <see cref="ControlState"/> on connect and a fresh snapshot (camelCase
/// JSON) on every change. Localhost-only by default. A malformed request is answered, never fatal.</summary>
public sealed class HttpControlServer : IAsyncDisposable
{
    private readonly IControlSurface _surface;
    private readonly HttpControlServerOptions _options;
    private readonly HttpControlRouter _router;
    private readonly ConcurrentDictionary<Guid, WebSocket> _sockets = new();
    // A WebSocket forbids concurrent sends; serialize all outbound frames (initial snapshot +
    // every broadcast) so two rapid state changes can't reset a connection.
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private HttpListener? _listener;
    private CancellationTokenSource? _cts;
    private Task? _acceptLoop;

    public HttpControlServer(IControlSurface surface, HttpControlServerOptions? options = null)
    {
        _surface = surface;
        _options = options ?? new HttpControlServerOptions();
        _router = new HttpControlRouter(surface);
    }

    public int ListenPort => _options.ListenPort;

    public void Start()
    {
        if (_listener is not null)
        {
            return;
        }

        _options.Validate();
        var listener = new HttpListener();
        try
        {
            listener.Prefixes.Add($"http://{_options.Host}:{_options.ListenPort}/");
            listener.Start();
        }
        catch
        {
            listener.Close();
            throw;
        }
        _listener = listener;
        _cts = new CancellationTokenSource();
        _surface.StateChanged += OnStateChanged;
        _acceptLoop = Task.Run(() => AcceptLoopAsync(_cts.Token));
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        var listener = _listener!;
        while (!cancellationToken.IsCancellationRequested)
        {
            HttpListenerContext context;
            try
            {
                context = await listener.GetContextAsync().ConfigureAwait(false);
            }
            catch (Exception) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (HttpListenerException)
            {
                break;  // listener stopped
            }
            catch (ObjectDisposedException)
            {
                break;
            }

            _ = HandleContextAsync(context, cancellationToken);
        }
    }

    private async Task HandleContextAsync(HttpListenerContext context, CancellationToken cancellationToken)
    {
        try
        {
            if (!IsAuthorized(context.Request))
            {
                await WriteResponseAsync(context, HttpControlResponse.Json(401, "{\"ok\":false,\"error\":\"Unauthorized\"}")).ConfigureAwait(false);
                return;
            }

            var path = context.Request.Url?.AbsolutePath ?? "/";
            if (context.Request.IsWebSocketRequest && path.TrimEnd('/') == "/ws")
            {
                await HandleWebSocketAsync(context, cancellationToken).ConfigureAwait(false);
                return;
            }

            string? body = null;
            if (context.Request.HasEntityBody)
            {
                using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding ?? Encoding.UTF8);
                body = await reader.ReadToEndAsync(cancellationToken).ConfigureAwait(false);
            }

            var response = await _router.HandleAsync(context.Request.HttpMethod, path, body, cancellationToken).ConfigureAwait(false);
            await WriteResponseAsync(context, response).ConfigureAwait(false);
        }
        catch
        {
            try
            {
                context.Response.StatusCode = 500;
                context.Response.Close();
            }
            catch
            {
                // response already gone
            }
        }
    }

    private bool IsAuthorized(HttpListenerRequest request)
    {
        if (string.IsNullOrWhiteSpace(_options.AuthToken))
        {
            return true;
        }

        var header = request.Headers["Authorization"];
        if (header is not null && header.StartsWith("Bearer ", StringComparison.OrdinalIgnoreCase) &&
            string.Equals(header["Bearer ".Length..], _options.AuthToken, StringComparison.Ordinal))
        {
            return true;
        }

        // Allow the token on the query string for the WS upgrade (browsers can't set WS headers).
        return request.IsWebSocketRequest && request.Url?.AbsolutePath.TrimEnd('/') == "/ws" &&
            string.Equals(request.QueryString["token"], _options.AuthToken, StringComparison.Ordinal);
    }

    private static async Task WriteResponseAsync(HttpListenerContext context, HttpControlResponse response)
    {
        var bytes = Encoding.UTF8.GetBytes(response.Body);
        context.Response.StatusCode = response.Status;
        context.Response.ContentType = response.ContentType;
        context.Response.ContentLength64 = bytes.Length;
        context.Response.Headers["Access-Control-Allow-Origin"] = "*";
        await context.Response.OutputStream.WriteAsync(bytes).ConfigureAwait(false);
        context.Response.Close();
    }

    private async Task HandleWebSocketAsync(HttpListenerContext context, CancellationToken cancellationToken)
    {
        HttpListenerWebSocketContext wsContext;
        try
        {
            wsContext = await context.AcceptWebSocketAsync(subProtocol: null).ConfigureAwait(false);
        }
        catch
        {
            context.Response.StatusCode = 400;
            context.Response.Close();
            return;
        }

        var socket = wsContext.WebSocket;
        var id = Guid.NewGuid();
        _sockets[id] = socket;
        try
        {
            // Send the current state immediately so a fresh client lights up without waiting.
            await SendStateAsync(socket, _surface.GetState(), cancellationToken).ConfigureAwait(false);

            // Drain incoming frames until close (we don't accept commands over WS in this phase).
            var buffer = new byte[1024];
            while (socket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
            {
                var result = await socket.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken).ConfigureAwait(false);
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    // Complete the close handshake so the client's CloseAsync resolves cleanly
                    // instead of seeing a reset.
                    await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, null, CancellationToken.None).ConfigureAwait(false);
                    break;
                }
            }
        }
        catch
        {
            // client vanished / cancelled
        }
        finally
        {
            _sockets.TryRemove(id, out _);
            try
            {
                if (socket.State == WebSocketState.Open)
                {
                    await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, null, CancellationToken.None).ConfigureAwait(false);
                }
            }
            catch
            {
            }

            socket.Dispose();
        }
    }

    private void OnStateChanged(object? sender, ControlState state)
    {
        _ = BroadcastAsync(state);
    }

    private async Task BroadcastAsync(ControlState state)
    {
        if (_sockets.IsEmpty)
        {
            return;
        }

        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(state, HttpControlRouter.JsonOptions));
        await _sendLock.WaitAsync().ConfigureAwait(false);
        try
        {
            foreach (var (id, socket) in _sockets)
            {
                try
                {
                    if (socket.State == WebSocketState.Open)
                    {
                        await socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, endOfMessage: true, CancellationToken.None).ConfigureAwait(false);
                    }
                }
                catch
                {
                    _sockets.TryRemove(id, out _);
                }
            }
        }
        finally
        {
            _sendLock.Release();
        }
    }

    private async Task SendStateAsync(WebSocket socket, ControlState state, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(state, HttpControlRouter.JsonOptions));
        await _sendLock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await socket.SendAsync(new ArraySegment<byte>(bytes), WebSocketMessageType.Text, endOfMessage: true, cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _sendLock.Release();
        }
    }

    public async ValueTask DisposeAsync()
    {
        _surface.StateChanged -= OnStateChanged;
        _cts?.Cancel();
        try
        {
            _listener?.Stop();
        }
        catch
        {
        }

        foreach (var (_, socket) in _sockets)
        {
            try
            {
                socket.Abort();
            }
            catch
            {
            }
        }

        _sockets.Clear();

        if (_acceptLoop is not null)
        {
            try
            {
                await _acceptLoop.ConfigureAwait(false);
            }
            catch
            {
            }
        }

        _listener?.Close();
        _cts?.Dispose();
        _listener = null;
    }
}
