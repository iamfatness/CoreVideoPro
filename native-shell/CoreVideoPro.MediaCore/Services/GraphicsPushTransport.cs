using System.Net;
using System.Net.WebSockets;
using System.Security.Cryptography.X509Certificates;

namespace CoreVideoPro.MediaCore.Services;

internal sealed class GraphicsPushTransport(
    Func<string, CancellationToken, Task<IPAddress[]>>? resolve = null,
    Func<IPAddress[], int, CancellationToken, ValueTask<Stream>>? dial = null,
    X509ChainPolicy? testTrust = null)
{
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    public async Task<GraphicsPushConnection> ConnectAsync(GraphicsFeedDefinition feed, GraphicsPushReceiveBudget budget,
        CancellationToken cancellation)
    {
        var target = GraphicsFeedTransport.ValidateWebSocketEndpoint(feed);
        var started = budget.Clock.GetTimestamp();
        using var deadline = new CancellationTokenSource(ConnectTimeout, budget.Clock);
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellation, deadline.Token);
        var handler = new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            UseProxy = false,
            UseCookies = false,
            AutomaticDecompression = DecompressionMethods.None,
            ConnectCallback = (context, token) => GraphicsFeedTransport.ConnectPublicAsync(context.DnsEndPoint,
                resolve ?? Dns.GetHostAddressesAsync, dial ?? GraphicsFeedTransport.DialAsync, token)
        };
        if (testTrust is not null) handler.SslOptions.CertificateChainPolicy = testTrust;
        var invoker = new HttpMessageInvoker(handler);
        var socket = new ClientWebSocket();
        socket.Options.CollectHttpResponseDetails = true;
        try
        {
            await socket.ConnectAsync(target, invoker, linked.Token).ConfigureAwait(false);
            linked.Token.ThrowIfCancellationRequested();
            if (budget.Clock.GetElapsedTime(started) >= ConnectTimeout)
                throw new TimeoutException("Graphics WebSocket connection timed out.");
            if (socket.HttpResponseHeaders?.Keys.Any(key => key.Equals("Sec-WebSocket-Extensions", StringComparison.OrdinalIgnoreCase)) == true)
                throw new InvalidDataException("Graphics WebSocket extensions are unsupported.");
            return new(socket, budget, invoker);
        }
        catch (Exception failure)
        {
            socket.Dispose();
            invoker.Dispose();
            cancellation.ThrowIfCancellationRequested();
            if (deadline.IsCancellationRequested) throw new TimeoutException("Graphics WebSocket connection timed out.");
            if (failure is WebSocketException socketFailure)
                throw new WebSocketException(socketFailure.WebSocketErrorCode, "Graphics WebSocket connection failed.");
            throw;
        }
    }
}

// Owned by the source, so replacing a socket cannot reset its rolling limits.
internal sealed class GraphicsPushReceiveBudget(TimeProvider clock)
{
    private static readonly TimeSpan Window = TimeSpan.FromSeconds(1);
    private readonly object _gate = new();
    private readonly Queue<(long Timestamp, int Bytes, bool Complete)> _recent = new();
    private int _bytes;
    private int _messages;

    internal TimeProvider Clock => clock;

    internal bool Charge(int bytes, bool complete)
    {
        lock (_gate)
        {
            var now = clock.GetTimestamp();
            while (_recent.TryPeek(out var oldest) && clock.GetElapsedTime(oldest.Timestamp, now) >= Window)
            {
                _recent.Dequeue();
                _bytes -= oldest.Bytes;
                if (oldest.Complete) _messages--;
            }
            if (bytes > 0 || complete) _recent.Enqueue((now, bytes, complete));
            _bytes += bytes;
            if (complete) _messages++;
            return _bytes <= 2 * GraphicsLocalData.ByteLimit && _messages <= 20;
        }
    }
}

internal sealed class GraphicsPushConnection(WebSocket socket, GraphicsPushReceiveBudget budget,
    IDisposable? owner = null) : IDisposable
{
    private static readonly TimeSpan AssemblyTimeout = TimeSpan.FromSeconds(10);
    private readonly byte[] _buffer = new byte[16 * 1024];

    // The source owns a single reader and passes its connection lifetime token.
    public async Task<byte[]?> ReadAsync(CancellationToken cancellation)
    {
        using var message = new MemoryStream();
        CancellationTokenSource? deadline = null;
        CancellationTokenSource? linked = null;
        long assemblyStarted = 0;
        try
        {
            for (var completions = 1; ; completions++)
            {
                var token = linked?.Token ?? cancellation;
                var part = await socket.ReceiveAsync(_buffer.AsMemory(), token).ConfigureAwait(false);
                var closed = part.MessageType == WebSocketMessageType.Close;
                var withinBudget = closed || budget.Charge(part.Count, part.EndOfMessage);
                token.ThrowIfCancellationRequested();
                if (deadline is not null && budget.Clock.GetElapsedTime(assemblyStarted) >= AssemblyTimeout)
                    throw new TimeoutException("Graphics WebSocket message assembly timed out.");
                if (closed) return null;
                if (!withinBudget)
                    throw new InvalidDataException("Graphics WebSocket receive budget exceeded.");
                if (part.MessageType != WebSocketMessageType.Text)
                    throw new InvalidDataException("Graphics WebSocket messages must be text.");
                if ((completions == 128 && !part.EndOfMessage) || message.Length + part.Count > GraphicsLocalData.ByteLimit)
                    throw new InvalidDataException("Graphics WebSocket message exceeds the assembly limit.");

                message.Write(_buffer, 0, part.Count);
                if (part.EndOfMessage) return message.ToArray();
                if (deadline is null)
                {
                    assemblyStarted = budget.Clock.GetTimestamp();
                    deadline = new(AssemblyTimeout, budget.Clock);
                    linked = CancellationTokenSource.CreateLinkedTokenSource(cancellation, deadline.Token);
                }
            }
        }
        catch (Exception failure)
        {
            socket.Abort();
            cancellation.ThrowIfCancellationRequested();
            if (deadline?.IsCancellationRequested == true)
                throw new TimeoutException("Graphics WebSocket message assembly timed out.");
            if (failure is WebSocketException socketFailure)
                throw new WebSocketException(socketFailure.WebSocketErrorCode, "Graphics WebSocket receive failed.");
            throw;
        }
        finally
        {
            linked?.Dispose();
            deadline?.Dispose();
        }
    }

    public void Dispose()
    {
        socket.Dispose();
        owner?.Dispose();
    }
}
