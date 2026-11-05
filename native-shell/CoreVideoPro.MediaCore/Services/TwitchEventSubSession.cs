using System.Globalization;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

internal enum TwitchChatFailure { Invalid, Authorization, Retry, Revoked }

internal sealed class TwitchChatException(TwitchChatFailure failure) : Exception(failure switch
{
    TwitchChatFailure.Authorization => "Twitch authorization is unavailable. Sign in again.",
    TwitchChatFailure.Revoked => "Twitch removed chat access. Review decisions cleared.",
    TwitchChatFailure.Retry => "Twitch chat disconnected. Review decisions cleared.",
    _ => "Twitch returned an unexpected response. Review decisions cleared."
})
{
    public TwitchChatFailure Failure { get; } = failure;
}

internal enum TwitchSocketEventKind { Notification, Keepalive, Reconnect, Revoked }
internal sealed record TwitchSocketEvent(TwitchSocketEventKind Kind, IReadOnlyList<SocialFeedEvent> Events);

internal static class TwitchJson
{
    internal static JsonElement Property(JsonElement value, string name) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(name, out var item) ? item : default;
    internal static string Text(JsonElement value, string name) =>
        Property(value, name) is { ValueKind: JsonValueKind.String } item ? item.GetString() ?? "" : "";
    internal static bool Identifier(string value) => value.Length is > 0 and <= 256 && value.All(c => c is >= '!' and <= '~');
    internal static bool UserId(string value) => value.Length is > 0 and <= 32 && value.All(char.IsAsciiDigit);
    internal static TwitchChatException Invalid() => new(TwitchChatFailure.Invalid);

    internal static JsonDocument Parse(string json)
    {
        if (Encoding.UTF8.GetByteCount(json) > 65536) throw Invalid();
        try { return JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 24 }); }
        catch (JsonException) { throw Invalid(); }
    }

    // Preserve Twitch's nanoseconds when a clear races an adjacent message.
    internal static long Timestamp(string value)
    {
        if (value.Length is < 20 or > 30 || value[^1] != 'Z' ||
            !DateTimeOffset.TryParseExact(value[..19], "yyyy-MM-dd'T'HH:mm:ss", CultureInfo.InvariantCulture,
                DateTimeStyles.AssumeUniversal, out var seconds)) throw Invalid();
        var fraction = value[19..^1];
        if (fraction.Length > 0)
        {
            if (fraction[0] != '.' || fraction.Length is < 2 or > 10 || !fraction[1..].All(char.IsAsciiDigit)) throw Invalid();
            fraction = fraction[1..];
        }
        try
        {
            var nanoseconds = fraction.Length == 0 ? 0 : long.Parse(fraction.PadRight(9, '0'), CultureInfo.InvariantCulture);
            var result = checked(seconds.ToUnixTimeSeconds() * 1_000_000_000L + nanoseconds);
            return result > 0 ? result : throw Invalid();
        }
        catch (OverflowException) { throw Invalid(); }
    }
}

/// <summary>One socket's verified subscriptions and bounded delivery deduplication.</summary>
internal sealed class TwitchEventSubSession
{
    // Establish removals before the message subscription can deliver text.
    internal static IReadOnlyList<string> RequiredTypes { get; } =
        ["channel.chat.clear", "channel.chat.clear_user_messages", "channel.chat.message_delete", "channel.chat.message"];
    private readonly string _channel, _user;
    private readonly Dictionary<string, string> _subscriptions = new(StringComparer.Ordinal);
    private readonly HashSet<string> _seen = new(StringComparer.Ordinal);
    private readonly Queue<string> _deliveries = new();
    internal string Id { get; }
    internal TimeSpan Keepalive { get; }
    internal bool IsReady => _subscriptions.Count == RequiredTypes.Count;

    private TwitchEventSubSession(string id, string channel, string user, int keepalive)
    {
        (Id, _channel, _user) = (id, channel, user);
        Keepalive = TimeSpan.FromSeconds(keepalive);
    }

    internal static TwitchEventSubSession Welcome(string json, string channel, string user)
    {
        using var document = TwitchJson.Parse(json);
        var metadata = TwitchJson.Property(document.RootElement, "metadata");
        var session = TwitchJson.Property(TwitchJson.Property(document.RootElement, "payload"), "session");
        var id = TwitchJson.Text(session, "id");
        if (TwitchJson.Text(metadata, "message_type") != "session_welcome" ||
            !TwitchJson.Identifier(TwitchJson.Text(metadata, "message_id")) ||
            !TwitchJson.Identifier(id) || !TwitchJson.UserId(channel) || !TwitchJson.UserId(user) ||
            TwitchJson.Text(session, "status") != "connected" ||
            TwitchJson.Property(session, "keepalive_timeout_seconds") is not { ValueKind: JsonValueKind.Number } timeout ||
            !timeout.TryGetInt32(out var seconds) || seconds is < 10 or > 600) throw TwitchJson.Invalid();
        TwitchJson.Timestamp(TwitchJson.Text(metadata, "message_timestamp"));
        return new(id, channel, user, seconds);
    }

    internal void Confirm(string type, string json)
    {
        using var document = TwitchJson.Parse(json);
        var rows = TwitchJson.Property(document.RootElement, "data");
        if (!RequiredTypes.Contains(type) || rows.ValueKind != JsonValueKind.Array || rows.GetArrayLength() != 1)
            throw TwitchJson.Invalid();
        Validate(rows[0], type, enabled: true);
        var id = TwitchJson.Text(rows[0], "id");
        if (_subscriptions.Any(entry => entry.Key != type && entry.Value == id)) throw TwitchJson.Invalid();
        _subscriptions[type] = id;
    }

    internal TwitchSocketEvent Read(string json)
    {
        if (!IsReady) throw TwitchJson.Invalid();
        using var document = TwitchJson.Parse(json);
        var metadata = TwitchJson.Property(document.RootElement, "metadata");
        var payload = TwitchJson.Property(document.RootElement, "payload");
        var delivery = TwitchJson.Text(metadata, "message_id");
        if (!TwitchJson.Identifier(delivery) || payload.ValueKind != JsonValueKind.Object) throw TwitchJson.Invalid();
        var order = TwitchJson.Timestamp(TwitchJson.Text(metadata, "message_timestamp"));
        var kind = TwitchJson.Text(metadata, "message_type");
        if (kind == "session_keepalive") return new(TwitchSocketEventKind.Keepalive, []);
        if (kind == "session_reconnect")
        {
            var session = TwitchJson.Property(payload, "session");
            if (TwitchJson.Text(session, "id") != Id || TwitchJson.Text(session, "status") != "reconnecting" ||
                !Uri.TryCreate(TwitchJson.Text(session, "reconnect_url"), UriKind.Absolute, out var target) ||
                target.Scheme != "wss" || target.IdnHost != "eventsub.wss.twitch.tv" || target.Port != 443 ||
                target.UserInfo.Length > 0 || target.Fragment.Length > 0 || target.AbsoluteUri.Length > 4096)
                throw TwitchJson.Invalid();
            return new(TwitchSocketEventKind.Reconnect, []);
        }
        if (kind is not ("notification" or "revocation")) throw TwitchJson.Invalid();
        var type = TwitchJson.Text(metadata, "subscription_type");
        var subscription = TwitchJson.Property(payload, "subscription");
        Validate(subscription, type, enabled: kind == "notification");
        if (TwitchJson.Text(metadata, "subscription_version") != "1" ||
            !_subscriptions.TryGetValue(type, out var expected) || TwitchJson.Text(subscription, "id") != expected)
            throw TwitchJson.Invalid();
        if (kind == "revocation") return new(TwitchSocketEventKind.Revoked, []);

        var item = TwitchJson.Property(payload, "event");
        if (TwitchJson.Text(item, "broadcaster_user_id") != _channel) throw TwitchJson.Invalid();
        var mapped = Map(type, item, order);
        if (!_seen.Add(delivery)) return new(TwitchSocketEventKind.Notification, []);
        _deliveries.Enqueue(delivery);
        if (_deliveries.Count > 4096) _seen.Remove(_deliveries.Dequeue());
        return new(TwitchSocketEventKind.Notification, mapped is null ? [] : [mapped]);
    }

    private SocialFeedEvent? Map(string type, JsonElement item, long order)
    {
        if (type == "channel.chat.clear") return new(SocialFeedEventKind.ClearRoom, Order: order);
        var author = TwitchJson.Text(item, type == "channel.chat.message" ? "chatter_user_id" : "target_user_id");
        if (!TwitchJson.UserId(author)) throw TwitchJson.Invalid();
        if (type == "channel.chat.clear_user_messages") return new(SocialFeedEventKind.ClearUser, AuthorId: author, Order: order);
        var id = TwitchJson.Text(item, "message_id");
        if (!TwitchJson.Identifier(id) || id.Length > 128) throw TwitchJson.Invalid();
        id = _channel + ":" + id;
        if (type == "channel.chat.message_delete") return new(SocialFeedEventKind.Remove, id, Order: order);
        // Shared-room origins and non-text products are outside the selected channel feed.
        var origin = TwitchJson.Text(item, "source_broadcaster_user_id");
        if (TwitchJson.Text(item, "message_type") != "text" || (origin.Length > 0 && origin != _channel)) return null;
        return new(SocialFeedEventKind.Text, id, author, TwitchJson.Text(item, "chatter_user_name"),
            TwitchJson.Text(TwitchJson.Property(item, "message"), "text"), order);
    }

    private void Validate(JsonElement subscription, string type, bool enabled)
    {
        var condition = TwitchJson.Property(subscription, "condition");
        var transport = TwitchJson.Property(subscription, "transport");
        if (!RequiredTypes.Contains(type) || TwitchJson.Text(subscription, "type") != type ||
            TwitchJson.Text(subscription, "version") != "1" || !TwitchJson.Identifier(TwitchJson.Text(subscription, "id")) ||
            (enabled && TwitchJson.Text(subscription, "status") != "enabled") ||
            TwitchJson.Text(condition, "broadcaster_user_id") != _channel || TwitchJson.Text(condition, "user_id") != _user ||
            TwitchJson.Text(transport, "method") != "websocket" || TwitchJson.Text(transport, "session_id") != Id)
            throw TwitchJson.Invalid();
    }
}
