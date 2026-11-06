using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class TwitchEventSubSessionTests
{
    internal const string Channel = "1234";
    internal const string User = "5678";
    internal const string Session = "fixture-session";
    internal static readonly string[] Types = ["channel.chat.clear", "channel.chat.clear_user_messages",
        "channel.chat.message_delete", "channel.chat.message"];

    internal static string Welcome(string id = Session, int keepalive = 10) => JsonSerializer.Serialize(new
    {
        metadata = new { message_id = "welcome", message_type = "session_welcome", message_timestamp = "2026-11-05T06:00:00.000000000Z" },
        payload = new { session = new { id, status = "connected", keepalive_timeout_seconds = keepalive } }
    });

    internal static object Subscription(string type, string channel = Channel, string user = User, string session = Session, string status = "enabled") => new
    {
        id = "sub-" + type, type, version = "1", status,
        condition = new { broadcaster_user_id = channel, user_id = user },
        transport = new { method = "websocket", session_id = session }
    };

    internal static string Confirmation(string type, string channel = Channel, string user = User, string session = Session) =>
        JsonSerializer.Serialize(new { data = new[] { Subscription(type, channel, user, session) } });

    internal static string Notification(string delivery, string type, object entry,
        string stamp = "2026-11-05T06:00:01.000000001Z", string channel = Channel) => JsonSerializer.Serialize(new
    {
        metadata = new { message_id = delivery, message_type = "notification", message_timestamp = stamp,
            subscription_type = type, subscription_version = "1" },
        payload = new { subscription = Subscription(type, channel), @event = entry }
    });

    internal static object Message(string id = "message-a", string author = "99", string text = "Synthetic message") => new
    {
        broadcaster_user_id = Channel, chatter_user_id = author, chatter_user_name = "Synthetic viewer",
        message_id = id, message_type = "text", message = new { text }
    };

    internal static TwitchEventSubSession Ready()
    {
        var session = TwitchEventSubSession.Welcome(Welcome(), Channel, User);
        foreach (var type in Types) session.Confirm(type, Confirmation(type));
        return session;
    }

    [Fact]
    public void ReviewRequiresAllFourMatchingSubscriptionsAndDeliveryIsDeduplicated()
    {
        var session = TwitchEventSubSession.Welcome(Welcome(), Channel, User);
        Assert.Equal(TimeSpan.FromSeconds(10), session.Keepalive);
        foreach (var type in Types[..3]) session.Confirm(type, Confirmation(type));
        Assert.False(session.IsReady);
        var wire = Notification("delivery-a", Types[3], Message());
        Assert.Throws<TwitchChatException>(() => session.Read(wire));
        session.Confirm(Types[3], Confirmation(Types[3]));
        Assert.True(session.IsReady);
        var item = Assert.Single(session.Read(wire).Events);
        Assert.Equal(SocialFeedEventKind.Text, item.Kind);
        Assert.Equal("1234:message-a", item.Id);
        Assert.Equal("99", item.AuthorId);
        Assert.Equal("Synthetic message", item.Text);
        Assert.Empty(session.Read(wire).Events);
    }

    [Theory]
    [InlineData("wrong", User, Session)]
    [InlineData(Channel, "wrong", Session)]
    [InlineData(Channel, User, "wrong")]
    public void ForeignSubscriptionCannotEnableReview(string channel, string user, string socket)
    {
        var session = TwitchEventSubSession.Welcome(Welcome(), Channel, User);
        Assert.Throws<TwitchChatException>(() => session.Confirm(Types[0], Confirmation(Types[0], channel, user, socket)));
        Assert.False(session.IsReady);
    }

    [Fact]
    public void DeletionAndClearEventsRetainNanosecondOrderingAndSelectedChannel()
    {
        var session = Ready();
        var removed = Assert.Single(session.Read(Notification("delete", Types[2], new
        { broadcaster_user_id = Channel, target_user_id = "99", message_id = "message-a" })).Events);
        Assert.Equal(SocialFeedEventKind.Remove, removed.Kind);
        Assert.Equal("1234:message-a", removed.Id);
        var userClear = Assert.Single(session.Read(Notification("clear-user", Types[1], new
        { broadcaster_user_id = Channel, target_user_id = "99" }, "2026-11-05T06:00:01.000000002Z")).Events);
        var roomClear = Assert.Single(session.Read(Notification("clear-room", Types[0], new
        { broadcaster_user_id = Channel }, "2026-11-05T06:00:01.000000003Z")).Events);
        Assert.Equal(SocialFeedEventKind.ClearUser, userClear.Kind);
        Assert.Equal(SocialFeedEventKind.ClearRoom, roomClear.Kind);
        Assert.Equal(1, roomClear.Order - userClear.Order);
        Assert.Throws<TwitchChatException>(() => session.Read(Notification("foreign", Types[3], Message(), channel: "other")));
    }

    [Fact]
    public void ClearFencesRetireOnlyPreClearItemsAndRejectDelayedDeliveries()
    {
        var session = Ready();
        var review = new GraphicsSocialReview();
        var source = review.Connect();
        void Apply(string wire)
        {
            foreach (var item in session.Read(wire).Events) review.Apply(source, item);
        }
        Apply(Notification("first", Types[3], Message("old")));
        var old = Assert.Single(review.Items);
        Assert.True(review.Approve(old.Key));
        Apply(Notification("newer", Types[3], Message("new"), "2026-11-05T06:00:03Z"));
        Apply(Notification("clear", Types[1], new { broadcaster_user_id = Channel, target_user_id = "99" }, "2026-11-05T06:00:02Z"));
        Assert.Null(review.Approved(old.Key));
        Assert.Equal("1234:new", Assert.Single(review.Items).Key.Id);
        Apply(Notification("late", Types[3], Message("late"), "2026-11-05T06:00:01.500000000Z"));
        Assert.Single(review.Items);
        Apply(Notification("room", Types[0], new { broadcaster_user_id = Channel }, "2026-11-05T06:00:04Z"));
        Assert.Empty(review.Items);
        Apply(Notification("late-room", Types[3], Message("late-room", "100"), "2026-11-05T06:00:03.999999999Z"));
        Assert.Empty(review.Items);
        Apply(Notification("after", Types[3], Message("after"), "2026-11-05T06:00:04.000000001Z"));
        Assert.Equal(GraphicsSocialDecision.Pending, Assert.Single(review.Items).Decision);
    }

    [Fact]
    public void RevocationAndPlannedMigrationAreExplicitAndInvalidEnvelopesFailClosed()
    {
        var session = Ready();
        var revoke = JsonSerializer.Serialize(new
        {
            metadata = new { message_id = "revoke", message_type = "revocation", message_timestamp = "2026-11-05T06:00:05Z",
                subscription_type = Types[0], subscription_version = "1" },
            payload = new { subscription = Subscription(Types[0], status: "authorization_revoked") }
        });
        Assert.Equal(TwitchSocketEventKind.Revoked, session.Read(revoke).Kind);
        var reconnect = JsonSerializer.Serialize(new
        {
            metadata = new { message_id = "move", message_type = "session_reconnect", message_timestamp = "2026-11-05T06:00:05Z" },
            payload = new { session = new { id = Session, status = "reconnecting", reconnect_url = "wss://eventsub.wss.twitch.tv/ws?fixture=next" } }
        });
        Assert.Equal(TwitchSocketEventKind.Reconnect, Ready().Read(reconnect).Kind);
        Assert.Throws<TwitchChatException>(() => Ready().Read("{}"));
        Assert.Throws<TwitchChatException>(() => Ready().Read(new string('x', 65537)));
        Assert.Throws<TwitchChatException>(() => TwitchEventSubSession.Welcome(Welcome(keepalive: 0), Channel, User));
    }
}
