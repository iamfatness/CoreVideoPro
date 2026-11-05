using Xunit;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsSocialReviewTests
{
    private static readonly DateTimeOffset Now = new(2026, 11, 4, 18, 0, 0, TimeSpan.Zero);

    [Fact]
    public void ApprovalAppliesOnlyToTheExactReviewedItemAndRevision()
    {
        var queue = new GraphicsSocialReview(() => Now);
        var source = queue.Connect();
        var first = queue.Receive(source, "first", "Author", "First message")!;
        var other = queue.Receive(source, "other", "Author", "Other message")!;

        Assert.Null(queue.Approved(first.Key));
        Assert.True(queue.Approve(first.Key));
        Assert.Equal("First message", queue.Approved(first.Key)?.Message);
        Assert.Null(queue.Approved(other.Key));
        Assert.Equal(first.Key, queue.Receive(source, "first", "Author", "First message")?.Key);
        Assert.NotNull(queue.Approved(first.Key));

        var edited = queue.Receive(source, "first", "Author", "Edited message")!;
        Assert.NotEqual(first.Key, edited.Key);
        Assert.False(queue.Approve(first.Key));
        Assert.False(queue.Reject(first.Key));
        Assert.Null(queue.Approved(first.Key));
        Assert.Null(queue.Approved(edited.Key));
        Assert.True(queue.Reject(edited.Key));
        Assert.False(queue.Approve(edited.Key));
        Assert.Equal(GraphicsSocialDecision.Rejected, queue.Receive(source, "first", "Author", "Edited message")?.Decision);
    }

    [Fact]
    public void RemovalBeforeArrivalAndRemovalAfterApprovalBothFenceReplays()
    {
        var queue = new GraphicsSocialReview(() => Now);
        var source = queue.Connect();
        queue.Withdraw(source, "early");
        Assert.Null(queue.Receive(source, "early", "Author", "Arrived after deletion"));
        var item = queue.Receive(source, "shown", "Author", "Message")!;
        Assert.True(queue.Approve(item.Key));
        queue.Withdraw(source, "shown");
        Assert.Null(queue.Approved(item.Key));
        Assert.Null(queue.Receive(source, "shown", "Author", "Message"));
        Assert.Empty(queue.Items);
    }

    [Fact]
    public void DisconnectAndReplacementFenceOldCallbacksAndDecisions()
    {
        var queue = new GraphicsSocialReview(() => Now);
        var firstSource = queue.Connect();
        var first = queue.Receive(firstSource, "id", "Author", "Message")!;
        Assert.True(queue.Approve(first.Key));
        queue.Disconnect();
        Assert.Null(queue.Approved(first.Key));
        Assert.Null(queue.Receive(firstSource, "old", "Author", "Late callback"));

        var secondSource = queue.Connect();
        var second = queue.Receive(secondSource, "id", "Author", "Message")!;
        queue.Withdraw(firstSource, "id");
        Assert.False(queue.Approve(first.Key));
        Assert.Null(queue.Approved(second.Key));
        Assert.True(queue.Approve(second.Key));
    }

    [Fact]
    public void ExpiryCannotBeExtendedByReplayAndTombstoneOverflowFailsClosed()
    {
        var time = Now;
        var queue = new GraphicsSocialReview(() => time, capacity: 2, removalCapacity: 1, lifetime: TimeSpan.FromSeconds(10));
        var source = queue.Connect();
        var first = queue.Receive(source, "first", "Author", "Message")!;
        Assert.True(queue.Approve(first.Key));
        time += TimeSpan.FromSeconds(9);
        Assert.Equal(first.Key, queue.Receive(source, "first", "Author", "Message")?.Key);
        var second = queue.Receive(source, "second", "Author", "Message")!;
        Assert.True(queue.Approve(second.Key));
        Assert.Null(queue.Receive(source, "third", "Author", "Queue full"));

        time += TimeSpan.FromSeconds(1);
        Assert.Null(queue.Approved(first.Key));
        Assert.Null(queue.Receive(source, "first", "Author", "Replay after expiry"));
        queue.Withdraw(source, "another");
        Assert.False(queue.IsConnected);
        Assert.Null(queue.Approved(second.Key));
        Assert.Null(queue.Receive(source, "new", "Author", "Late callback"));
    }

    [Fact]
    public void ReviewAndApprovalUseTheSameBoundedPlainText()
    {
        var queue = new GraphicsSocialReview(() => Now);
        var source = queue.Connect();
        Assert.Null(queue.Receive(source, "bad", "Author", "Bad\0text"));
        Assert.Null(queue.Receive(source, "long", "Author", new string('x', 513)));
        var item = queue.Receive(source, "valid", " Author ", " Cafe\u0301 ")!;
        Assert.Equal("Author", item.Author);
        Assert.Equal("Café", item.Message);
        Assert.True(queue.Approve(item.Key));
        Assert.Equal(item.Message, queue.Approved(item.Key)?.Message);
    }
}
