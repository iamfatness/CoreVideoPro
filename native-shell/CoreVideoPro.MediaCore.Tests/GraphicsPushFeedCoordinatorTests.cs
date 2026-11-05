using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsPushFeedCoordinatorTests
{
    private static readonly GraphicsFeedDefinition Feed = new("feed", "Fixture", "wss", "wss://feed.example.com/live", "id", Enabled: true);

    [Fact]
    public void ReplayConfirmsFreshnessButStaleAndMalformedMessagesDoNot()
    {
        using var rig = new Rig();
        rig.Tick();
        rig.Sockets[0].Send(4);
        rig.Tick();
        Assert.Equal(rig.Clock.GetUtcNow(), rig.Coordinator.Status.LastSuccess);
        rig.Tick(10);
        rig.Sockets[0].Send(4);
        rig.Tick();
        var fresh = rig.Clock.GetUtcNow();
        Assert.Equal(fresh, rig.Coordinator.Status.LastSuccess);

        rig.Tick(15);
        rig.Sockets[0].Send(3);
        rig.Tick();
        Assert.Equal(fresh, rig.Coordinator.Status.LastSuccess);
        rig.Sockets[0].Deliver("malformed"u8.ToArray());
        rig.Tick(15);
        Assert.Equal("stale", rig.Coordinator.Status.State);
        Assert.Equal(fresh, rig.Coordinator.Status.LastSuccess);
        Assert.Equal("invalid-data", rig.Coordinator.Status.Failure);
        rig.Tick(30);
        Assert.True(rig.Sockets[0].Token.IsCancellationRequested);
        Assert.Single(rig.Sockets);
        rig.Tick(5);
        Assert.Equal(2, rig.Sockets.Count);
    }

    [Fact]
    public async Task BackoffGrowsAndManualReconnectHonorsTheFiveSecondFloor()
    {
        using var rig = new Rig();
        rig.Tick();
        await rig.CompleteAsync(0);
        rig.Tick();
        Assert.Equal(rig.Clock.GetUtcNow().AddSeconds(5), rig.Coordinator.Status.NextCheck);
        rig.Tick(5);
        await rig.CompleteAsync(1);
        rig.Tick();
        Assert.Equal(rig.Clock.GetUtcNow().AddSeconds(10), rig.Coordinator.Status.NextCheck);
        rig.Coordinator.ReconnectNow();
        rig.Tick(4);
        Assert.Equal(2, rig.Sockets.Count);
        rig.Tick(1);
        Assert.Equal(3, rig.Sockets.Count);
        rig.Sockets[2].Send(7);
        rig.Tick();
        await rig.CompleteAsync(2);
        rig.Tick();
        Assert.Equal(rig.Clock.GetUtcNow().AddSeconds(5), rig.Coordinator.Status.NextCheck);
    }

    [Fact]
    public void SlowCommitKeepsOnlyTheHighestWaitingSequenceAndHonorsCadence()
    {
        var pending = new TaskCompletionSource<GraphicsFeedStage>();
        using var rig = new Rig((sequence, _) => sequence == 1 ? pending.Task : Task.FromResult(GraphicsFeedStage.Updated));
        rig.Tick();
        var socket = rig.Sockets[0];
        socket.Send(1);
        rig.Tick();
        socket.Send(2);
        socket.Send(4);
        socket.Send(3);
        rig.Tick();
        Assert.Equal(new long[] { 1 }, rig.Commits);
        pending.SetResult(GraphicsFeedStage.Updated);
        rig.Tick();
        Assert.Equal(new long[] { 1 }, rig.Commits);
        rig.Tick(1);
        Assert.Equal(new long[] { 1, 4 }, rig.Commits);
    }

    [Fact]
    public void IncompatibleSchemaDoesNotReplaceAValidWaitingSnapshot()
    {
        using var rig = new Rig();
        var data = GraphicsLocalData.Parse("""[{"id":"a","line":"Text"}]""", "json", "id");
        rig.Coordinator.Configure(Feed, new GraphicsDataSource("feed", "Fixture", 1, rig.Clock.GetUtcNow().AddSeconds(-30), data)
        {
            FeedSequence = 3
        });
        Assert.Equal("stale", rig.Coordinator.Status.State);
        Assert.Null(rig.Coordinator.Status.LastSuccess);
        rig.Tick();
        var socket = rig.Sockets[0];
        socket.Send(4);
        socket.Deliver("""{"schemaVersion":1,"sequence":5,"rows":[{"id":"a","line":5}]}"""u8.ToArray());
        rig.Tick();
        Assert.Equal(new long[] { 4 }, rig.Commits);
        Assert.Equal(4, rig.Coordinator.Status.AcceptedSequence);
        Assert.Equal("fresh", rig.Coordinator.Status.State);
        Assert.Equal("invalid-data", rig.Coordinator.Status.Failure);
    }

    [Fact]
    public void ShutdownDuringConnectIgnoresACompletionThatArrivesAfterCancellation()
    {
        var clock = new TestClock();
        var connect = new TaskCompletionSource();
        Action? connected = null;
        Action<byte[]>? deliver = null;
        CancellationToken lifetime = default;
        using var coordinator = new GraphicsPushFeedCoordinator((_, _, onConnect, onRead, token) =>
        {
            (connected, deliver, lifetime) = (onConnect, onRead, token);
            return connect.Task;
        }, (_, _, _, _) => throw new InvalidOperationException("Unexpected stage."), clock, work => work());
        coordinator.Configure(Feed);
        coordinator.Tick();
        Assert.Equal("connecting", coordinator.Status.State);
        coordinator.Dispose();
        Assert.True(lifetime.IsCancellationRequested);
        connected!();
        deliver!("""{"schemaVersion":1,"sequence":1,"rows":[{"id":"a"}]}"""u8.ToArray());
        connect.SetResult();
        coordinator.Tick();
        Assert.Equal("disabled", coordinator.Status.State);
    }

    [Fact]
    public void DisableAndReplacementFenceOldReadsAndWaitForAnOldWrite()
    {
        var pending = new TaskCompletionSource<GraphicsFeedStage>();
        using var rig = new Rig((sequence, token) =>
        {
            if (sequence != 1) return Task.FromResult(GraphicsFeedStage.Updated);
            Assert.False(token.IsCancellationRequested);
            return pending.Task;
        });
        rig.Tick();
        var old = rig.Sockets[0];
        old.Send(1);
        rig.Tick();
        rig.Coordinator.Configure(Feed with { Enabled = false, Generation = 1 });
        Assert.True(old.Token.IsCancellationRequested);
        rig.Coordinator.Configure(Feed with { Generation = 2 });
        rig.Tick(5);
        var replacement = rig.Sockets[1];
        old.Send(99);
        old.Complete();
        replacement.Send(2);
        rig.Tick();
        Assert.Equal(new long[] { 1 }, rig.Commits);
        pending.SetResult(GraphicsFeedStage.Updated);
        rig.Tick();
        Assert.Equal(new long[] { 1, 2 }, rig.Commits);
        Assert.False(replacement.Token.IsCancellationRequested);
        rig.Coordinator.Dispose();
        Assert.True(replacement.Token.IsCancellationRequested);
    }

    [Fact]
    public void FailedCommitDoesNotAcknowledgeAndTheSameSequenceCanBeRetried()
    {
        var attempts = 0;
        using var rig = new Rig((_, _) => Task.FromResult(++attempts == 1 ? GraphicsFeedStage.Rejected : GraphicsFeedStage.Updated));
        rig.Tick();
        var socket = rig.Sockets[0];
        socket.Send(8);
        rig.Tick();
        Assert.Null(rig.Coordinator.Status.LastSuccess);
        Assert.Equal("rejected", rig.Coordinator.Status.Failure);
        rig.Tick(1);
        socket.Send(8);
        rig.Tick();
        Assert.Equal(new long[] { 8, 8 }, rig.Commits);
        Assert.Equal(rig.Clock.GetUtcNow(), rig.Coordinator.Status.LastSuccess);
    }

    private sealed class Rig : IDisposable
    {
        public TestClock Clock { get; } = new();
        public List<Socket> Sockets { get; } = [];
        public List<long> Commits { get; } = [];
        public GraphicsPushFeedCoordinator Coordinator { get; }
        private long _watermark;
        private readonly List<Task> _readers = [];

        public Rig(Func<long, CancellationToken, Task<GraphicsFeedStage>>? stage = null)
        {
            Coordinator = new((_, _, connected, deliver, token) =>
            {
                var socket = new Socket(deliver, token);
                Sockets.Add(socket);
                connected();
                return socket.Lifetime.Task;
            }, (_, snapshot, _, token) =>
            {
                Commits.Add(snapshot.Sequence);
                if (stage is not null) return stage(snapshot.Sequence, token);
                if (snapshot.Sequence < _watermark) return Task.FromResult(GraphicsFeedStage.StaleSequence);
                if (snapshot.Sequence == _watermark) return Task.FromResult(GraphicsFeedStage.Unchanged);
                _watermark = snapshot.Sequence;
                return Task.FromResult(GraphicsFeedStage.Updated);
            }, Clock, work =>
            {
                var reader = work();
                _readers.Add(reader);
                return reader;
            });
            Coordinator.Configure(Feed);
        }

        public void Tick(int seconds = 0)
        {
            Clock.Seconds += seconds;
            Coordinator.Tick();
        }

        public Task CompleteAsync(int index)
        {
            Sockets[index].Complete();
            return _readers[index];
        }

        public void Dispose()
        {
            Coordinator.Dispose();
            foreach (var socket in Sockets) socket.Complete();
        }
    }

    private sealed class Socket(Action<byte[]> deliver, CancellationToken token)
    {
        public CancellationToken Token => token;
        public TaskCompletionSource Lifetime { get; } = new();
        public void Complete() => Lifetime.TrySetResult();
        public void Deliver(byte[] bytes) => deliver(bytes);
        public void Send(long sequence) => Deliver(Encoding.UTF8.GetBytes(
            $$"""{"schemaVersion":1,"sequence":{{sequence}},"rows":[{"id":"a","line":"Text"}]}"""));
    }

    private sealed class TestClock : TimeProvider
    {
        public int Seconds { get; set; }
        public override long TimestampFrequency => 1;
        public override long GetTimestamp() => Seconds;
        public override DateTimeOffset GetUtcNow() => DateTimeOffset.UnixEpoch.AddSeconds(Seconds);
    }
}
