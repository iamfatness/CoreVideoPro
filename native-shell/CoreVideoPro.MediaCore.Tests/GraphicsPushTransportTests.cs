using System.Net;
using System.Net.WebSockets;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsPushTransportTests
{
    private static readonly GraphicsFeedDefinition Feed = new("feed", "Fixture", "wss", "wss://feed.example.com/live", "id");
    private static readonly IPAddress PublicAddress = IPAddress.Parse("8.8.8.8");

    [Theory]
    [InlineData("ws://feed.example.com/live")]
    [InlineData("wss://feed.example.com:8443/live")]
    [InlineData("wss://127.0.0.1/live")]
    [InlineData("wss://[::ffff:10.0.0.1]/live")]
    [InlineData("wss://feed.example.com/live?key=value")]
    [InlineData("wss://user@feed.example.com/live")]
    [InlineData("wss://feed.example.com/live#fragment")]
    public async Task DeniedTargetsNeverResolveOrDial(string endpoint)
    {
        var transport = new GraphicsPushTransport((_, _) => throw new Xunit.Sdk.XunitException("Unexpected resolution."),
            (_, _, _) => throw new Xunit.Sdk.XunitException("Unexpected dial."));
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            transport.ConnectAsync(Feed with { Endpoint = endpoint }, new(TimeProvider.System), default));
    }

    [Theory]
    [InlineData("10.0.0.1")]
    [InlineData("::ffff:10.0.0.1")]
    public async Task EachDialUsesOnlyItsVettedAddressesAndRejectsMixedRebinding(string privateAddress)
    {
        var resolutions = 0;
        var dials = 0;
        var transport = new GraphicsPushTransport((host, _) =>
        {
            Assert.Equal("feed.example.com", host);
            return Task.FromResult(++resolutions == 1 ? [PublicAddress] :
                new[] { PublicAddress, IPAddress.Parse(privateAddress) });
        }, (addresses, port, _) =>
        {
            Assert.Equal(PublicAddress, Assert.Single(addresses));
            Assert.Equal(443, port);
            dials++;
            throw new IOException("Synthetic dial failure.");
        });
        var budget = new GraphicsPushReceiveBudget(TimeProvider.System);
        await Assert.ThrowsAsync<WebSocketException>(() => transport.ConnectAsync(Feed, budget, default));
        await Assert.ThrowsAsync<WebSocketException>(() => transport.ConnectAsync(Feed, budget, default));
        Assert.Equal(2, resolutions);
        Assert.Equal(1, dials);
    }

    [Fact]
    public async Task ReadsOneCompleteTextMessageAndThenRecognizesClose()
    {
        using var socket = new FramedSocket();
        socket.Add("one ", end: false);
        socket.Add("message");
        socket.Add("", WebSocketMessageType.Close);
        using var reader = new GraphicsPushConnection(socket, new(TimeProvider.System));
        Assert.Equal("one message", Encoding.UTF8.GetString((await reader.ReadAsync(default))!));
        Assert.Null(await reader.ReadAsync(default));
    }

    [Fact]
    public async Task RejectsBinaryAndAbortsTheConnection()
    {
        using var socket = new FramedSocket();
        socket.Add("bytes", WebSocketMessageType.Binary);
        using var reader = new GraphicsPushConnection(socket, new(TimeProvider.System));
        await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(default));
        Assert.True(socket.Aborted);
    }

    [Theory]
    [InlineData(16384, 65)]
    [InlineData(1, 129)]
    public async Task BoundsBytesAndReceiveCompletionsPerMessage(int frameBytes, int frames)
    {
        using var socket = new FramedSocket();
        for (var i = 0; i < frames; i++) socket.Add(new string('x', frameBytes), end: i == frames - 1);
        using var reader = new GraphicsPushConnection(socket, new(TimeProvider.System));
        await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(default));
        Assert.True(socket.Aborted);
    }

    [Fact]
    public async Task SourceBudgetSurvivesReconnectAndExpiresOnTheRollingClock()
    {
        var clock = new TestClock();
        var budget = new GraphicsPushReceiveBudget(clock);
        using var first = new FramedSocket();
        for (var i = 0; i < 20; i++) first.Add("discarded");
        using (var reader = new GraphicsPushConnection(first, budget))
            for (var i = 0; i < 20; i++) Assert.NotNull(await reader.ReadAsync(default));

        clock.Advance(TimeSpan.FromMilliseconds(999));
        using var second = new FramedSocket();
        second.Add("over budget");
        using (var reader = new GraphicsPushConnection(second, budget))
            await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(default));

        clock.Advance(TimeSpan.FromMilliseconds(1));
        using var third = new FramedSocket();
        third.Add("within budget");
        using var resumed = new GraphicsPushConnection(third, budget);
        Assert.NotNull(await resumed.ReadAsync(default));
    }

    [Fact]
    public async Task ReconnectDoesNotDiscardBytesFromAnUnfinishedMessage()
    {
        var budget = new GraphicsPushReceiveBudget(new TestClock());
        var large = new string('x', GraphicsLocalData.ByteLimit);
        using var first = new FramedSocket();
        first.Add(large);
        first.Add(large, end: false);
        first.Add("", WebSocketMessageType.Close);
        using (var reader = new GraphicsPushConnection(first, budget))
        {
            Assert.Equal(GraphicsLocalData.ByteLimit, (await reader.ReadAsync(default))!.Length);
            Assert.Null(await reader.ReadAsync(default));
        }

        using var second = new FramedSocket();
        second.Add("x");
        using var resumed = new GraphicsPushConnection(second, budget);
        var rejected = await Assert.ThrowsAsync<InvalidDataException>(() => resumed.ReadAsync(default));
        Assert.Contains("receive budget", rejected.Message);
        Assert.True(second.Aborted);
    }

    [Fact]
    public async Task FragmentAssemblyTimesOutOnTheInjectedClock()
    {
        var clock = new TestClock();
        using var socket = new FramedSocket();
        socket.Add("partial", end: false);
        using var reader = new GraphicsPushConnection(socket, new(clock));
        var read = reader.ReadAsync(default);
        clock.Advance(TimeSpan.FromMilliseconds(9_999));
        Assert.False(read.IsCompleted);
        clock.Advance(TimeSpan.FromMilliseconds(1));
        await Assert.ThrowsAsync<TimeoutException>(() => read);
        Assert.True(socket.Aborted);
    }

    [Fact]
    public async Task ConnectDeadlineIncludesResolution()
    {
        var clock = new TestClock();
        var transport = new GraphicsPushTransport(async (_, cancellation) =>
        {
            await Task.Delay(Timeout.InfiniteTimeSpan, cancellation);
            return [PublicAddress];
        }, (_, _, _) => throw new Xunit.Sdk.XunitException("Unexpected dial."));
        var connect = transport.ConnectAsync(Feed, new(clock), default);
        clock.Advance(TimeSpan.FromMilliseconds(9_999));
        Assert.False(connect.IsCompleted);
        clock.Advance(TimeSpan.FromMilliseconds(1));
        await Assert.ThrowsAsync<TimeoutException>(() => connect);
    }

    [Fact]
    public async Task InFlightReceiveObservesCancellation()
    {
        using var socket = new FramedSocket();
        socket.Add("partial", end: false);
        using var reader = new GraphicsPushConnection(socket, new(TimeProvider.System));
        using var lifetime = new CancellationTokenSource();
        var read = reader.ReadAsync(lifetime.Token);
        Assert.False(read.IsCompleted);
        lifetime.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => read);
    }

    private sealed class TestClock : TimeProvider
    {
        private long _timestamp;
        private TestTimer? _timer;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;
        public override long GetTimestamp() => _timestamp;
        public override ITimer CreateTimer(TimerCallback callback, object? state, TimeSpan dueTime, TimeSpan period) =>
            _timer = new(callback, state, _timestamp + dueTime.Ticks);

        public void Advance(TimeSpan duration)
        {
            _timestamp += duration.Ticks;
            _timer?.FireAt(_timestamp);
        }

        private sealed class TestTimer(TimerCallback callback, object? state, long deadline) : ITimer
        {
            private bool _disposed;
            public void FireAt(long timestamp)
            {
                if (_disposed || timestamp < deadline) return;
                _disposed = true;
                callback(state);
            }
            public void Dispose() => _disposed = true;
            public ValueTask DisposeAsync() { Dispose(); return ValueTask.CompletedTask; }
            public bool Change(TimeSpan dueTime, TimeSpan period) => throw new NotSupportedException();
        }
    }

    private sealed class FramedSocket : WebSocket
    {
        private readonly Queue<(byte[] Bytes, WebSocketMessageType Type, bool End)> _frames = new();
        private int _offset;
        public bool Aborted { get; private set; }
        public override WebSocketState State => Aborted ? WebSocketState.Aborted : WebSocketState.Open;
        public override string? SubProtocol => null;
        public override WebSocketCloseStatus? CloseStatus => null;
        public override string? CloseStatusDescription => null;
        public void Add(string text, WebSocketMessageType type = WebSocketMessageType.Text, bool end = true) =>
            _frames.Enqueue((Encoding.UTF8.GetBytes(text), type, end));

        public override async ValueTask<ValueWebSocketReceiveResult> ReceiveAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            if (_frames.Count == 0) await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
            var (bytes, type, end) = _frames.Peek();
            var count = Math.Min(buffer.Length, bytes.Length - _offset);
            bytes.AsMemory(_offset, count).CopyTo(buffer);
            _offset += count;
            if (_offset != bytes.Length) return new(count, type, false);
            _frames.Dequeue();
            _offset = 0;
            return new(count, type, end);
        }

        public override void Abort() => Aborted = true;
        public override void Dispose() { }
        public override Task CloseAsync(WebSocketCloseStatus status, string? description, CancellationToken cancellationToken) => throw new NotSupportedException();
        public override Task CloseOutputAsync(WebSocketCloseStatus status, string? description, CancellationToken cancellationToken) => throw new NotSupportedException();
        public override Task<WebSocketReceiveResult> ReceiveAsync(ArraySegment<byte> buffer, CancellationToken cancellationToken) => throw new NotSupportedException();
        public override Task SendAsync(ArraySegment<byte> buffer, WebSocketMessageType type, bool end, CancellationToken cancellationToken) => throw new NotSupportedException();
    }
}
