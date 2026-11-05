using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

internal sealed record TwitchTokenPair(string AccessToken, string RefreshToken, int ExpiresIn);
internal sealed record TwitchDeviceGrant(string DeviceCode, TwitchDevicePrompt Prompt, int Interval);
internal sealed record TwitchIdentity(string UserId, string Login, int ExpiresIn);
internal sealed class TwitchDevicePendingException(bool slowDown) : Exception("Twitch authorization is pending.")
{
    internal bool SlowDown { get; } = slowDown;
}

internal sealed class TwitchHttpTransport(
    Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> send, IDisposable? owner = null) : IDisposable
{
    internal const string ReadScope = "user:read:chat";
    internal static TwitchHttpTransport Create()
    {
        var client = new HttpClient(new SocketsHttpHandler {
            AllowAutoRedirect = false, UseCookies = false, UseProxy = false,
            PooledConnectionLifetime = TimeSpan.FromMinutes(5)
        }) { Timeout = Timeout.InfiniteTimeSpan };
        return new((request, ct) => client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct), client);
    }

    public void Dispose() => owner?.Dispose();
    internal static bool Token(string value) => value.Length is >= 10 and <= 4096 && value.All(c => c is >= '!' and <= '~');
    internal static bool Login(string value) => value.Length is > 0 and <= 25 && value.All(c => char.IsAsciiLetterOrDigit(c) || c == '_');
    internal static bool ClientId(string value) => value.Length is >= 10 and <= 128 && value.All(char.IsAsciiLetterOrDigit);

    internal async Task<TwitchDeviceGrant> BeginAsync(string clientId, DateTimeOffset now, CancellationToken cancellation)
    {
        using var request = Form("device", new() { ["client_id"] = clientId, ["scopes"] = ReadScope });
        using var document = await RequestAsync(request, cancellation).ConfigureAwait(false);
        var root = document.RootElement;
        var device = TwitchJson.Text(root, "device_code");
        var code = TwitchJson.Text(root, "user_code");
        if (!Token(device) || !TwitchJson.Identifier(code) || code.Length > 32 ||
            !Uri.TryCreate(TwitchJson.Text(root, "verification_uri"), UriKind.Absolute, out var uri) ||
            uri.Scheme != "https" || uri.IdnHost != "www.twitch.tv" || uri.Port != 443 || uri.AbsolutePath != "/activate" ||
            uri.UserInfo.Length > 0 || uri.Fragment.Length > 0 || uri.AbsoluteUri.Length > 2048)
            throw TwitchJson.Invalid();
        var expires = Positive(root, "expires_in");
        var interval = Positive(root, "interval");
        if (expires > 3600 || interval > 60) throw TwitchJson.Invalid();
        return new(device, new(code, uri, now.AddSeconds(expires)), interval);
    }

    internal async Task<TwitchTokenPair> PollAsync(string clientId, string deviceCode, CancellationToken cancellation)
    {
        using var request = Form("token", new() { ["client_id"] = clientId, ["scopes"] = ReadScope,
            ["device_code"] = deviceCode, ["grant_type"] = "urn:ietf:params:oauth:grant-type:device_code" });
        using var document = await RequestAsync(request, cancellation, pending: true).ConfigureAwait(false);
        return Tokens(document.RootElement);
    }

    internal async Task<TwitchTokenPair> RefreshAsync(TwitchCredentials credentials, CancellationToken cancellation)
    {
        using var request = Form("token", new() { ["client_id"] = credentials.ClientId,
            ["refresh_token"] = credentials.RefreshToken, ["grant_type"] = "refresh_token" });
        using var document = await RequestAsync(request, cancellation).ConfigureAwait(false);
        return Tokens(document.RootElement);
    }

    internal async Task<TwitchIdentity> ValidateAsync(TwitchCredentials credentials, CancellationToken cancellation)
    {
        if (!ClientId(credentials.ClientId) || !Token(credentials.AccessToken) || !Token(credentials.RefreshToken))
            throw new TwitchChatException(TwitchChatFailure.Authorization);
        using var request = new HttpRequestMessage(HttpMethod.Get, "https://id.twitch.tv/oauth2/validate");
        request.Headers.Authorization = new AuthenticationHeaderValue("OAuth", credentials.AccessToken);
        using var document = await RequestAsync(request, cancellation).ConfigureAwait(false);
        var root = document.RootElement;
        var user = TwitchJson.Text(root, "user_id");
        var login = TwitchJson.Text(root, "login");
        if (TwitchJson.Text(root, "client_id") != credentials.ClientId || !TwitchJson.UserId(user) ||
            !Login(login) || !HasReadScope(root, "scopes"))
            throw new TwitchChatException(TwitchChatFailure.Authorization);
        return new(user, login, Positive(root, "expires_in"));
    }

    internal async Task<string> ChannelAsync(string login, TwitchCredentials credentials, CancellationToken cancellation)
    {
        if (!Login(login)) throw TwitchJson.Invalid();
        using var request = Helix(HttpMethod.Get, "users?login=" + Uri.EscapeDataString(login.ToLowerInvariant()), credentials);
        using var document = await RequestAsync(request, cancellation).ConfigureAwait(false);
        var rows = TwitchJson.Property(document.RootElement, "data");
        if (rows.ValueKind != JsonValueKind.Array || rows.GetArrayLength() != 1 ||
            !string.Equals(TwitchJson.Text(rows[0], "login"), login, StringComparison.OrdinalIgnoreCase) ||
            !TwitchJson.UserId(TwitchJson.Text(rows[0], "id"))) throw TwitchJson.Invalid();
        return TwitchJson.Text(rows[0], "id");
    }

    internal async Task SubscribeAsync(TwitchEventSubSession session, string type, string channel,
        TwitchCredentials credentials, CancellationToken cancellation)
    {
        if (!TwitchEventSubSession.RequiredTypes.Contains(type)) throw TwitchJson.Invalid();
        using var request = Helix(HttpMethod.Post, "eventsub/subscriptions", credentials);
        request.Content = new StringContent(JsonSerializer.Serialize(new {
            type, version = "1", condition = new { broadcaster_user_id = channel, user_id = credentials.UserId },
            transport = new { method = "websocket", session_id = session.Id }
        }), Encoding.UTF8, "application/json");
        using var document = await RequestAsync(request, cancellation).ConfigureAwait(false);
        session.Confirm(type, document.RootElement.GetRawText());
    }

    private static HttpRequestMessage Form(string path, Dictionary<string, string> fields) =>
        new(HttpMethod.Post, "https://id.twitch.tv/oauth2/" + path) { Content = new FormUrlEncodedContent(fields) };

    private static HttpRequestMessage Helix(HttpMethod method, string path, TwitchCredentials credentials)
    {
        var request = new HttpRequestMessage(method, "https://api.twitch.tv/helix/" + path);
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", credentials.AccessToken);
        request.Headers.Add("Client-Id", credentials.ClientId);
        return request;
    }

    private async Task<JsonDocument> RequestAsync(HttpRequestMessage request, CancellationToken cancellation, bool pending = false)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(TimeSpan.FromSeconds(12));
        try
        {
            using var response = await send(request, deadline.Token).ConfigureAwait(false);
            if ((int)response.StatusCode is >= 300 and < 400) throw new TwitchChatException(TwitchChatFailure.Authorization);
            var bytes = await GraphicsFeedTransport.ReadBody(response.Content, deadline.Token).ConfigureAwait(false);
            var document = TwitchJson.Parse(Encoding.UTF8.GetString(bytes));
            if (response.IsSuccessStatusCode) return document;
            using (document)
            {
                var message = TwitchJson.Text(document.RootElement, "message");
                if (pending && response.StatusCode == HttpStatusCode.BadRequest && message is "authorization_pending" or "slow_down")
                    throw new TwitchDevicePendingException(message == "slow_down");
                throw new TwitchChatException((int)response.StatusCode >= 500 || response.StatusCode == HttpStatusCode.TooManyRequests
                    ? TwitchChatFailure.Retry : TwitchChatFailure.Authorization);
            }
        }
        catch (OperationCanceledException) when (!cancellation.IsCancellationRequested)
        { throw new TwitchChatException(TwitchChatFailure.Retry); }
        catch (InvalidDataException) { throw TwitchJson.Invalid(); }
        catch (Exception error) when (error is HttpRequestException or IOException)
        { throw new TwitchChatException(TwitchChatFailure.Retry); }
    }

    private static TwitchTokenPair Tokens(JsonElement root)
    {
        var access = TwitchJson.Text(root, "access_token");
        var refresh = TwitchJson.Text(root, "refresh_token");
        if (!Token(access) || !Token(refresh) || TwitchJson.Text(root, "token_type") != "bearer" || !HasReadScope(root, "scope"))
            throw new TwitchChatException(TwitchChatFailure.Authorization);
        return new(access, refresh, Positive(root, "expires_in"));
    }

    private static bool HasReadScope(JsonElement root, string name) =>
        TwitchJson.Property(root, name) is { ValueKind: JsonValueKind.Array } scopes && scopes.GetArrayLength() <= 32 &&
        scopes.EnumerateArray().Any(item => item.ValueKind == JsonValueKind.String && item.GetString() == ReadScope);

    private static int Positive(JsonElement root, string name) =>
        TwitchJson.Property(root, name) is { ValueKind: JsonValueKind.Number } value && value.TryGetInt32(out var number) && number > 0
            ? number : throw TwitchJson.Invalid();
}
