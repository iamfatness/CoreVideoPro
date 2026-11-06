using System.Net.WebSockets;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class TwitchChatSocketTests
{
    private sealed class Wire(params (byte[] Bytes, WebSocketMessageType Kind, bool End)[] frames) : WebSocket
    {
        private int _index;
        public override WebSocketCloseStatus? CloseStatus => null;
        public override string? CloseStatusDescription => null;
        public override WebSocketState State => WebSocketState.Open;
        public override string? SubProtocol => null;
        public override void Abort() { }
        public override void Dispose() { }
        public override Task CloseAsync(WebSocketCloseStatus status, string? description, CancellationToken cancellation) => throw new NotSupportedException();
        public override Task CloseOutputAsync(WebSocketCloseStatus status, string? description, CancellationToken cancellation) => throw new NotSupportedException();
        public override Task SendAsync(ArraySegment<byte> buffer, WebSocketMessageType kind, bool end, CancellationToken cancellation) => throw new NotSupportedException();
        public override async Task<WebSocketReceiveResult> ReceiveAsync(ArraySegment<byte> buffer, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            if (frames.Length == 0) await Task.Delay(Timeout.InfiniteTimeSpan, cancellation);
            var frame = frames[_index++];
            frame.Bytes.AsSpan().CopyTo(buffer.AsSpan());
            return new WebSocketReceiveResult(frame.Bytes.Length, frame.Kind, frame.End);
        }
    }

    [Fact]
    public async Task FragmentedUtf8IsDecodedOnlyAfterTheWholeMessage()
    {
        using var socket = new TwitchChatSocket(new Wire(
            ([0x7b, 0xc3], WebSocketMessageType.Text, false), ([0xa9, 0x7d], WebSocketMessageType.Text, true)));
        Assert.Equal("{é}", await socket.ReadAsync(TimeSpan.FromSeconds(10), CancellationToken.None));
    }

    [Fact]
    public async Task MissedKeepaliveDeadlineBecomesAConnectionGap()
    {
        using var socket = new TwitchChatSocket(new Wire());
        var failure = await Assert.ThrowsAsync<TwitchChatException>(() =>
            socket.ReadAsync(TimeSpan.Zero, CancellationToken.None).WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(TwitchChatFailure.Retry, failure.Failure);
    }

    [Theory]
    [InlineData("binary")]
    [InlineData("utf8")]
    [InlineData("oversize")]
    [InlineData("closed")]
    public async Task InvalidOrClosedFramesFailClosed(string scenario)
    {
        var bytes = scenario == "utf8" ? new byte[] { 0xc3 } : Encoding.UTF8.GetBytes("{}");
        var kind = scenario == "binary" ? WebSocketMessageType.Binary : scenario == "closed" ? WebSocketMessageType.Close : WebSocketMessageType.Text;
        var frames = scenario == "oversize"
            ? Enumerable.Range(0, 9).Select(_ => (new byte[8192], WebSocketMessageType.Text, false)).ToArray()
            : new[] { (bytes, kind, true) };
        using var socket = new TwitchChatSocket(new Wire(frames));
        var error = await Assert.ThrowsAsync<TwitchChatException>(() => socket.ReadAsync(TimeSpan.FromSeconds(10), CancellationToken.None));
        Assert.Equal(scenario == "closed" ? TwitchChatFailure.Retry : TwitchChatFailure.Invalid, error.Failure);
    }
}
