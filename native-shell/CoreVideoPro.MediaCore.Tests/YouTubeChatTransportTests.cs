using System.Net;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class YouTubeChatTransportTests
{
    private const string Key = "fixture-api-key-abcdefghijklmnop";

    [Fact]
    public async Task ProviderRequestsUseFixedTlsHostAndMapExplicitRemovalsAndText()
    {
        var steps = new Queue<string>([
            """{"items":[{"id":"abcDEF123_4","status":{"privacyStatus":"public"},"liveStreamingDetails":{"activeLiveChatId":"chat-id"}}]}""",
            """
            {"pollingIntervalMillis":21000,"nextPageToken":"next+/","items":[
              {"id":"message","snippet":{"type":"textMessageEvent","liveChatId":"chat-id","authorChannelId":"channel","textMessageDetails":{"messageText":"Synthetic text"}},"authorDetails":{"displayName":"Synthetic author"}},
              {"id":"delete-event","snippet":{"type":"messageDeletedEvent","liveChatId":"chat-id","messageDeletedDetails":{"deletedMessageId":"deleted"}}},
              {"id":"retract-event","snippet":{"type":"messageRetractedEvent","liveChatId":"chat-id","messageRetractedDetails":{"retractedMessageId":"retracted"}}},
              {"id":"tombstone","snippet":{"type":"tombstone","liveChatId":"chat-id"}},
              {"id":"ban","snippet":{"type":"userBannedEvent","liveChatId":"chat-id","userBannedDetails":{"bannedUserDetails":{"channelId":"banned-author"}}}},
              {"id":"ignored","snippet":{"type":"superChatEvent","liveChatId":"chat-id"}}
            ]}
            """
        ]);
        using var transport = new YouTubeChatTransport((request, _) => {
            Assert.Equal("www.googleapis.com", request.RequestUri!.Host);
            Assert.Equal("https", request.RequestUri.Scheme);
            Assert.DoesNotContain(Key, request.RequestUri.AbsoluteUri);
            Assert.Equal(Key, Assert.Single(request.Headers.GetValues("x-goog-api-key")));
            if (steps.Count == 1) Assert.Contains("pageToken=previous%2B%2F", request.RequestUri.AbsoluteUri);
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(steps.Dequeue()) });
        });
        Assert.Equal("chat-id", await transport.ResolveChatAsync("abcDEF123_4", Key, default));
        var page = await transport.ReadPageAsync("chat-id", "previous+/", Key, default);
        Assert.Equal("next+/", page.Cursor);
        Assert.Equal(TimeSpan.FromSeconds(21), page.Interval);
        Assert.Equal(new[] { SocialFeedEventKind.Text, SocialFeedEventKind.Remove, SocialFeedEventKind.Remove,
            SocialFeedEventKind.Remove, SocialFeedEventKind.Ban }, page.Events.Select(e => e.Kind));
        Assert.Equal("Synthetic author", page.Events[0].Author);
        Assert.Equal("Synthetic text", page.Events[0].Text);
        Assert.Equal("channel", page.Events[0].AuthorId);
        Assert.Equal("deleted", page.Events[1].Id);
        Assert.Equal("retracted", page.Events[2].Id);
        Assert.Equal("tombstone", page.Events[3].Id);
        Assert.Equal("banned-author", page.Events[4].AuthorId);
        Assert.Empty(steps);
    }

    [Theory]
    [InlineData(HttpStatusCode.Redirect, "", YouTubeChatFailure.Authorization)]
    [InlineData(HttpStatusCode.Unauthorized, "", YouTubeChatFailure.Authorization)]
    [InlineData(HttpStatusCode.Forbidden, "quotaExceeded", YouTubeChatFailure.Retry)]
    [InlineData(HttpStatusCode.Forbidden, "liveChatEnded", YouTubeChatFailure.Ended)]
    [InlineData(HttpStatusCode.BadRequest, "invalidPageToken", YouTubeChatFailure.Retry)]
    public async Task FailuresAreClassifiedWithoutExposingTheResponseOrKey(HttpStatusCode status, string reason,
        YouTubeChatFailure expected)
    {
        var body = System.Text.Json.JsonSerializer.Serialize(new { error = new { message = Key, errors = new[] { new { reason } } } });
        using var transport = new YouTubeChatTransport((_, _) => Task.FromResult(new HttpResponseMessage(status) { Content = new StringContent(body) }));
        var error = await Assert.ThrowsAsync<YouTubeChatException>(() => transport.ReadPageAsync("chat-id", null, Key, default));
        Assert.Equal(expected, error.Failure);
        Assert.DoesNotContain(Key, error.Message);
    }

    [Fact]
    public async Task WrongSourceOrMissingRemovalTargetFailsClosed()
    {
        const string missing = """{"pollingIntervalMillis":1000,"nextPageToken":"next","items":[{"id":"event","snippet":{"type":"messageDeletedEvent","liveChatId":"chat-id"}}]}""";
        using var transport = new YouTubeChatTransport((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(missing) }));
        var error = await Assert.ThrowsAsync<YouTubeChatException>(() => transport.ReadPageAsync("chat-id", null, Key, default));
        Assert.Equal(YouTubeChatFailure.Invalid, error.Failure);
    }
    [Fact]
    public async Task CursorAdvancesAndReconnectRequiresReviewAgain()
    {
        const string lookup = """{"items":[{"id":"abcDEF123_4","status":{"privacyStatus":"public"},"liveStreamingDetails":{"activeLiveChatId":"chat-id"}}]}""";
        const string first = """{"pollingIntervalMillis":1000,"nextPageToken":"cursor","items":[{"id":"msg","snippet":{"type":"textMessageEvent","liveChatId":"chat-id","authorChannelId":"author-id","textMessageDetails":{"messageText":"Text"}},"authorDetails":{"displayName":"Author"}}]}""";
        var responses = new Queue<(HttpStatusCode Status, string Body)>([
            (HttpStatusCode.OK, lookup), (HttpStatusCode.OK, first), (HttpStatusCode.ServiceUnavailable, ""),
            (HttpStatusCode.OK, lookup), (HttpStatusCode.OK, first)]);
        var requests = new List<string>();
        using var transport = new YouTubeChatTransport((request, _) =>
        {
            requests.Add(request.RequestUri!.Query);
            var (status, body) = responses.Dequeue();
            return Task.FromResult(new HttpResponseMessage(status) { Content = new StringContent(body) });
        });
        var review = new GraphicsSocialPlayout((_, _, _) => throw new InvalidOperationException("Approval alone sends no native commands."), () => null);
        using var cancellation = new CancellationTokenSource();
        var waits = new List<TimeSpan>();
        GraphicsSocialItemKey previous = default;
        await YouTubeSocialFeed.RunAsync("abcDEF123_4", Key, review, transport, _ => { }, (wait, _) =>
        {
            waits.Add(wait);
            switch (waits.Count)
            {
                case 1:
                    previous = Assert.Single(review.Items).Key;
                    Assert.True(review.Approve(previous));
                    break;
                case 2:
                    Assert.False(review.IsConnected);
                    Assert.Empty(review.Items);
                    break;
                case 3:
                    var replayed = Assert.Single(review.Items);
                    Assert.Equal(GraphicsSocialDecision.Pending, replayed.Decision);
                    Assert.False(review.Approve(previous));
                    cancellation.Cancel();
                    break;
            }
            return Task.CompletedTask;
        }, cancellation.Token);
        Assert.Equal(new[] { 15d, 30d, 15d }, waits.Select(wait => wait.TotalSeconds));
        Assert.Contains("pageToken=cursor", requests[2]);
        Assert.DoesNotContain("pageToken=", requests[4]);
        Assert.False(review.IsConnected);
        Assert.Empty(review.Items);
        Assert.Empty(responses);
    }

}
