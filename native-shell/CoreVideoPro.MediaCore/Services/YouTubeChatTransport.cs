using System.Net;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public enum YouTubeChatFailure { Authorization, Ended, Retry, Invalid }

internal sealed class YouTubeChatException(YouTubeChatFailure failure) : Exception(failure switch
{
    YouTubeChatFailure.Authorization => "YouTube rejected the API key or the request.",
    YouTubeChatFailure.Ended => "This public livestream has no active chat.",
    YouTubeChatFailure.Retry => "YouTube chat is temporarily unavailable.",
    _ => "YouTube returned an unexpected chat response."
})
{
    public YouTubeChatFailure Failure { get; } = failure;
}

internal enum SocialFeedEventKind { Text, Remove, Ban }
internal sealed record SocialFeedEvent(SocialFeedEventKind Kind, string Id = "", string AuthorId = "",
    string Author = "", string Text = "");
internal sealed record YouTubeChatPage(string Cursor, TimeSpan Interval, IReadOnlyList<SocialFeedEvent> Events);

internal sealed class YouTubeChatTransport(
    Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send, IDisposable? owner = null) : IDisposable
{
    private const string Origin = "https://www.googleapis.com/youtube/v3/";
    private const string PageFields = "pollingIntervalMillis,nextPageToken,offlineAt," +
        "items(id,snippet(type,liveChatId,authorChannelId,textMessageDetails,messageDeletedDetails,messageRetractedDetails,userBannedDetails),authorDetails(displayName))";

    public static YouTubeChatTransport Create()
    {
        var client = new HttpClient(new SocketsHttpHandler {
            AllowAutoRedirect = false, UseCookies = false, UseProxy = false,
            PooledConnectionLifetime = TimeSpan.FromMinutes(5)
        }) { Timeout = Timeout.InfiniteTimeSpan };
        return new((request, ct) => client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct), client);
    }

    public void Dispose() => owner?.Dispose();

    internal static bool IsVideoId(string value) => value.Length == 11 && value.All(IsIdentifierCharacter);
    internal static bool IsApiKey(string value) => value.Length is >= 20 and <= 128 && value.All(IsIdentifierCharacter);
    private static bool IsIdentifierCharacter(char value) => char.IsAsciiLetterOrDigit(value) || value is '-' or '_';
    private static bool IsProviderValue(string value, int limit = 256) =>
        value.Length > 0 && value.Length <= limit && value.All(c => c is >= '!' and <= '~');

    public async Task<string> ResolveChatAsync(string videoId, string apiKey, CancellationToken cancellation)
    {
        if (!IsVideoId(videoId)) throw new YouTubeChatException(YouTubeChatFailure.Invalid);
        var query = $"videos?part=liveStreamingDetails,status&id={videoId}&fields=items(id,status/privacyStatus,liveStreamingDetails/activeLiveChatId)";
        using var document = await FetchAsync(query, apiKey, cancellation).ConfigureAwait(false);
        if (Property(document.RootElement, "items") is not { ValueKind: JsonValueKind.Array } items)
            throw new YouTubeChatException(YouTubeChatFailure.Invalid);
        if (items.GetArrayLength() == 0) throw new YouTubeChatException(YouTubeChatFailure.Ended);
        if (items.GetArrayLength() != 1 || Text(items[0], "id") != videoId)
            throw new YouTubeChatException(YouTubeChatFailure.Invalid);
        var chatId = Text(Property(items[0], "liveStreamingDetails"), "activeLiveChatId");
        if (Text(Property(items[0], "status"), "privacyStatus") != "public" || chatId.Length == 0)
            throw new YouTubeChatException(YouTubeChatFailure.Ended);
        if (!IsProviderValue(chatId)) throw new YouTubeChatException(YouTubeChatFailure.Invalid);
        return chatId;
    }

    public async Task<YouTubeChatPage> ReadPageAsync(string chatId, string? cursor, string apiKey, CancellationToken cancellation)
    {
        if (!IsProviderValue(chatId) || (cursor is not null && !IsProviderValue(cursor, 2048)))
            throw new YouTubeChatException(YouTubeChatFailure.Invalid);
        var query = $"liveChat/messages?part=id,snippet,authorDetails&maxResults=200&liveChatId={Uri.EscapeDataString(chatId)}" +
            "&fields=" + Uri.EscapeDataString(PageFields);
        if (cursor is not null) query += "&pageToken=" + Uri.EscapeDataString(cursor);
        using var document = await FetchAsync(query, apiKey, cancellation).ConfigureAwait(false);
        var page = document.RootElement;
        if (Text(page, "offlineAt").Length > 0) throw new YouTubeChatException(YouTubeChatFailure.Ended);
        var next = Text(page, "nextPageToken");
        if (!IsProviderValue(next, 2048) || Property(page, "pollingIntervalMillis") is not { ValueKind: JsonValueKind.Number } interval ||
            !interval.TryGetInt32(out var milliseconds) || milliseconds <= 0 ||
            Property(page, "items") is not { ValueKind: JsonValueKind.Array } rows || rows.GetArrayLength() > 200)
            throw new YouTubeChatException(YouTubeChatFailure.Invalid);
        var events = new List<SocialFeedEvent>(rows.GetArrayLength());
        foreach (var row in rows.EnumerateArray())
        {
            var snippet = Property(row, "snippet");
            var kind = Text(snippet, "type");
            if (Text(snippet, "liveChatId") != chatId || kind.Length == 0)
                throw new YouTubeChatException(YouTubeChatFailure.Invalid);
            if (kind == "chatEndedEvent") throw new YouTubeChatException(YouTubeChatFailure.Ended);
            var mapped = kind switch
            {
                "textMessageEvent" => new SocialFeedEvent(SocialFeedEventKind.Text, Text(row, "id"),
                    Text(snippet, "authorChannelId"), Text(Property(row, "authorDetails"), "displayName"),
                    Text(Property(snippet, "textMessageDetails"), "messageText")),
                "messageDeletedEvent" => new(SocialFeedEventKind.Remove,
                    Text(Property(snippet, "messageDeletedDetails"), "deletedMessageId")),
                "messageRetractedEvent" => new(SocialFeedEventKind.Remove,
                    Text(Property(snippet, "messageRetractedDetails"), "retractedMessageId")),
                "tombstone" => new(SocialFeedEventKind.Remove, Text(row, "id")),
                "userBannedEvent" => new(SocialFeedEventKind.Ban,
                    AuthorId: Text(Property(Property(snippet, "userBannedDetails"), "bannedUserDetails"), "channelId")),
                _ => null
            };
            if (mapped is null) continue;
            if ((mapped.Kind != SocialFeedEventKind.Ban && !IsProviderValue(mapped.Id)) ||
                (mapped.Kind != SocialFeedEventKind.Remove && !IsProviderValue(mapped.AuthorId)))
                throw new YouTubeChatException(YouTubeChatFailure.Invalid);
            events.Add(mapped);
        }
        return new(next, TimeSpan.FromMilliseconds(Math.Max(milliseconds, 15000)), events);
    }

    private async Task<JsonDocument> FetchAsync(string query, string apiKey, CancellationToken cancellation)
    {
        if (!IsApiKey(apiKey)) throw new YouTubeChatException(YouTubeChatFailure.Authorization);
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromSeconds(12));
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, Origin + query);
            request.Headers.Add("x-goog-api-key", apiKey);
            using var response = await send(request, deadline.Token).ConfigureAwait(false);
            if ((int)response.StatusCode is >= 300 and < 400)
                throw new YouTubeChatException(YouTubeChatFailure.Authorization);
            var bytes = await GraphicsFeedTransport.ReadBody(response.Content, deadline.Token).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
                throw new YouTubeChatException(FailureOf(response.StatusCode, bytes));
            return JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 20 });
        }
        catch (OperationCanceledException) when (!cancellation.IsCancellationRequested)
        { throw new YouTubeChatException(YouTubeChatFailure.Retry); }
        catch (Exception error) when (error is InvalidDataException or JsonException)
        { throw new YouTubeChatException(YouTubeChatFailure.Invalid); }
        catch (Exception error) when (error is HttpRequestException or IOException)
        { throw new YouTubeChatException(YouTubeChatFailure.Retry); }
    }

    private static YouTubeChatFailure FailureOf(HttpStatusCode status, byte[] bytes)
    {
        if ((int)status >= 500 || status == HttpStatusCode.TooManyRequests) return YouTubeChatFailure.Retry;
        string reason = "";
        try
        {
            using var error = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 20 });
            var errors = Property(Property(error.RootElement, "error"), "errors");
            if (errors.ValueKind == JsonValueKind.Array && errors.GetArrayLength() > 0)
                reason = Text(errors[0], "reason");
        }
        catch (JsonException) { }
        return reason switch
        {
            "liveChatEnded" or "liveChatDisabled" or "liveChatNotFound" => YouTubeChatFailure.Ended,
            "quotaExceeded" or "rateLimitExceeded" or "userRateLimitExceeded" or "invalidPageToken" => YouTubeChatFailure.Retry,
            _ => YouTubeChatFailure.Authorization
        };
    }

    private static JsonElement Property(JsonElement value, string name) =>
        value.ValueKind == JsonValueKind.Object && value.TryGetProperty(name, out var property) ? property : default;

    private static string Text(JsonElement value, string name) =>
        Property(value, name) is { ValueKind: JsonValueKind.String } property ? property.GetString() ?? "" : "";
}
