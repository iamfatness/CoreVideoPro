using System.Net;
using System.Text.Json;
using System.Threading.Channels;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class TwitchSocialFeedTests
{
    private sealed class Socket : ITwitchChatSocket
    {
        private readonly Channel<string> _wire = Channel.CreateUnbounded<string>();
        private readonly Dictionary<int, TaskCompletionSource> _reads = [];
        private int _count;
        public bool Disposed;
        public List<TimeSpan> Deadlines = [];
        public Socket(string session = TwitchEventSubSessionTests.Session) => Post(TwitchEventSubSessionTests.Welcome(session));
        public void Post(string value) => Assert.True(_wire.Writer.TryWrite(value));
        public void Lose() => _wire.Writer.TryComplete();
        public Task Reading(int number)
        {
            lock (_reads)
            {
                if (_count >= number) return Task.CompletedTask;
                if (!_reads.TryGetValue(number, out var ready)) _reads[number] = ready = new(TaskCreationOptions.RunContinuationsAsynchronously);
                return ready.Task.WaitAsync(TimeSpan.FromSeconds(5));
            }
        }
        public async Task<string> ReadAsync(TimeSpan timeout, CancellationToken cancellation)
        {
            lock (_reads)
            {
                Deadlines.Add(timeout);
                ++_count;
                if (_reads.TryGetValue(_count, out var ready)) ready.TrySetResult();
            }
            try { return await _wire.Reader.ReadAsync(cancellation); }
            catch (ChannelClosedException) { throw new TwitchChatException(TwitchChatFailure.Retry); }
        }
        public void Dispose() { Disposed = true; _wire.Writer.TryComplete(); }
    }

    private sealed class Fixture : IDisposable
    {
        public readonly TwitchAuthorizationTests.Clock Clock = new();
        public readonly GraphicsSocialPlayout Playout = new((_, _, _) => throw new Exception("Unapproved command"), () => null);
        public readonly List<string> Reports = [];
        public readonly List<string> SubscriptionOrder = [];
        public readonly CancellationTokenSource Stop = new();
        public readonly TaskCompletionSource SubscribingText = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly TaskCompletionSource AllowText = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public readonly TaskCompletionSource Retry = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public bool PauseText, RefuseText;
        public int Connections;
        public readonly Socket Socket = new();
        public readonly TwitchAuthorization Auth;
        public Fixture()
        {
            TwitchCredentials? saved = TwitchAuthorizationTests.Tokens(Clock);
            var transport = new TwitchHttpTransport(async (request, cancellation) =>
            {
                if (request.RequestUri!.Host == "id.twitch.tv") return TwitchAuthorizationTests.Json(TwitchAuthorizationTests.Validation());
                Assert.Equal("api.twitch.tv", request.RequestUri.Host);
                Assert.Equal(TwitchAuthorizationTests.Client, Assert.Single(request.Headers.GetValues("Client-Id")));
                if (request.Method == HttpMethod.Get) return TwitchAuthorizationTests.Json("""{"data":[{"id":"1234","login":"fixturechannel"}]}""");
                using var body = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(cancellation));
                var type = body.RootElement.GetProperty("type").GetString()!;
                var condition = body.RootElement.GetProperty("condition");
                Assert.Equal("1234", condition.GetProperty("broadcaster_user_id").GetString());
                Assert.Equal("5678", condition.GetProperty("user_id").GetString());
                SubscriptionOrder.Add(type);
                if (type == "channel.chat.message")
                {
                    SubscribingText.TrySetResult();
                    if (PauseText) await AllowText.Task.WaitAsync(cancellation);
                    if (RefuseText) return TwitchAuthorizationTests.Json("{}", HttpStatusCode.Forbidden);
                }
                return TwitchAuthorizationTests.Json(TwitchEventSubSessionTests.Confirmation(type), HttpStatusCode.Accepted);
            });
            Auth = new(transport, () => saved, value => saved = value, () => saved = null, Clock, Clock.Delay);
        }
        public Task Run() => TwitchSocialFeed.RunAsync("fixturechannel", TwitchAuthorizationTests.Client, Auth, Playout,
            _ => { ++Connections; return Task.FromResult<ITwitchChatSocket>(Socket); },
            status => { lock (Reports) Reports.Add(status); }, _ => Assert.Fail("Unexpected device prompt"),
            async (span, ct) => {
                if (span != TimeSpan.FromSeconds(30)) Retry.TrySetResult();
                await Task.Delay(Timeout.InfiniteTimeSpan, ct);
            }, Stop.Token);
        public void Dispose() { Stop.Cancel(); Stop.Dispose(); Auth.Dispose(); }
    }

    [Fact]
    public async Task RemovalSubscriptionsAreReadyBeforeAnyMessageCanEnterReview()
    {
        using var fixture = new Fixture { PauseText = true };
        var running = fixture.Run();
        await fixture.SubscribingText.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(fixture.Playout.IsConnected);
        fixture.AllowText.SetResult();
        await fixture.Socket.Reading(2);
        fixture.Socket.Post(TwitchEventSubSessionTests.Notification("a", "channel.chat.message", TwitchEventSubSessionTests.Message("a")));
        fixture.Socket.Post(TwitchEventSubSessionTests.Notification("b", "channel.chat.message", TwitchEventSubSessionTests.Message("b")));
        await fixture.Socket.Reading(4);
        Assert.Equal(TwitchEventSubSessionTests.Types, fixture.SubscriptionOrder);
        Assert.Equal(2, fixture.Playout.Items.Count);
        Assert.All(fixture.Playout.Items, item => Assert.Equal(GraphicsSocialDecision.Pending, item.Decision));
        fixture.Stop.Cancel();
        await running.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Empty(fixture.Playout.Items);
        Assert.True(fixture.Socket.Disposed);
    }

    [Fact]
    public async Task FailedRequiredSubscriptionNeverEnablesReview()
    {
        using var fixture = new Fixture { RefuseText = true };
        await fixture.Run().WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(fixture.Playout.IsConnected);
        Assert.Empty(fixture.Playout.Items);
        Assert.True(fixture.Socket.Disposed);
    }

    [Fact]
    public async Task RevocationClearsAnApprovedItemWithoutRetry()
    {
        using var fixture = new Fixture();
        var running = fixture.Run();
        await fixture.Socket.Reading(2);
        Assert.Equal(TimeSpan.FromSeconds(10), fixture.Socket.Deadlines[0]);
        Assert.Equal(TimeSpan.FromSeconds(10), fixture.Socket.Deadlines[1]);
        fixture.Socket.Post(TwitchEventSubSessionTests.Notification("a", "channel.chat.message", TwitchEventSubSessionTests.Message("a")));
        await fixture.Socket.Reading(3);
        var item = Assert.Single(fixture.Playout.Items);
        Assert.True(fixture.Playout.Approve(item.Key));
        var revoked = TwitchEventSubSessionTests.Notification("revoked", "channel.chat.message", TwitchEventSubSessionTests.Message("a"))
            .Replace("\"message_type\":\"notification\"", "\"message_type\":\"revocation\"")
            .Replace("\"status\":\"enabled\"", "\"status\":\"authorization_revoked\"");
        fixture.Socket.Post(revoked);
        await running.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(fixture.Playout.IsConnected);
        Assert.Empty(fixture.Playout.Items);
        Assert.False(fixture.Playout.Approve(item.Key));
        Assert.Equal(1, fixture.Connections);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task UnexpectedDisconnectAndPlannedMigrationClearApprovals(bool planned)
    {
        using var fixture = new Fixture();
        var running = fixture.Run();
        await fixture.Socket.Reading(2);
        fixture.Socket.Post(TwitchEventSubSessionTests.Notification("a", "channel.chat.message", TwitchEventSubSessionTests.Message("a")));
        await fixture.Socket.Reading(3);
        var item = Assert.Single(fixture.Playout.Items);
        Assert.True(fixture.Playout.Approve(item.Key));
        if (planned)
        {
            fixture.Socket.Post(JsonSerializer.Serialize(new
            {
                metadata = new { message_id = "move", message_type = "session_reconnect", message_timestamp = "2026-11-05T06:00:05Z" },
                payload = new { session = new { id = TwitchEventSubSessionTests.Session, status = "reconnecting",
                    reconnect_url = "wss://eventsub.wss.twitch.tv/ws?fixture=next" } }
            }));
        }
        else fixture.Socket.Lose();
        await fixture.Retry.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(fixture.Playout.IsConnected);
        Assert.Empty(fixture.Playout.Items);
        Assert.False(fixture.Playout.Approve(item.Key));
        fixture.Stop.Cancel();
        await running.WaitAsync(TimeSpan.FromSeconds(5));
    }
}
