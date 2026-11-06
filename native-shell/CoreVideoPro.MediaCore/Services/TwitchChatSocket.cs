using System.Net.WebSockets;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

internal interface ITwitchChatSocket : IDisposable
{
    Task<string> ReadAsync(TimeSpan timeout, CancellationToken cancellation);
}

/// <summary>Receives bounded text frames from the canonical Twitch endpoint.</summary>
internal sealed class TwitchChatSocket(WebSocket socket, IDisposable? owner = null) : ITwitchChatSocket
{
    private int _disposed;
    internal static async Task<ITwitchChatSocket> ConnectAsync(CancellationToken cancellation)
    {
        var socket = new ClientWebSocket();
        var invoker = new HttpMessageInvoker(new SocketsHttpHandler {
            AllowAutoRedirect = false, UseCookies = false, UseProxy = false
        });
        socket.Options.KeepAliveInterval = TimeSpan.Zero;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromSeconds(12));
        try
        {
            await socket.ConnectAsync(new Uri("wss://eventsub.wss.twitch.tv/ws"), invoker, deadline.Token).ConfigureAwait(false);
            return new TwitchChatSocket(socket, invoker);
        }
        catch (Exception error) when (error is WebSocketException or HttpRequestException or OperationCanceledException)
        {
            socket.Dispose();
            invoker.Dispose();
            cancellation.ThrowIfCancellationRequested();
            throw new TwitchChatException(TwitchChatFailure.Retry);
        }
    }

    public async Task<string> ReadAsync(TimeSpan timeout, CancellationToken cancellation)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(timeout);
        using var body = new MemoryStream();
        var buffer = new byte[8192];
        try
        {
            while (true)
            {
                var frame = await socket.ReceiveAsync(buffer.AsMemory(), deadline.Token).ConfigureAwait(false);
                if (frame.MessageType == WebSocketMessageType.Close) throw new TwitchChatException(TwitchChatFailure.Retry);
                if (frame.MessageType != WebSocketMessageType.Text || body.Length + frame.Count > 65536) throw TwitchJson.Invalid();
                body.Write(buffer, 0, frame.Count);
                if (frame.EndOfMessage) return new UTF8Encoding(false, true).GetString(body.GetBuffer(), 0, (int)body.Length);
            }
        }
        catch (DecoderFallbackException) { throw TwitchJson.Invalid(); }
        catch (Exception error) when (error is WebSocketException or IOException or OperationCanceledException)
        {
            cancellation.ThrowIfCancellationRequested();
            throw new TwitchChatException(TwitchChatFailure.Retry);
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;
        socket.Abort();
        socket.Dispose();
        owner?.Dispose();
    }
}
