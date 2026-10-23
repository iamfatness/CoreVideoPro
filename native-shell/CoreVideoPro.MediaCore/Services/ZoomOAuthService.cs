using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class ZoomOAuthSessionStatus
{
    public bool BrokerConfigured { get; init; }
    public bool SignedIn { get; init; }
    public long ExpiresAt { get; init; }
    public bool PendingAuthorization { get; init; }
}

public sealed class ZoomJoinCredentials
{
    public string? SdkJwt { get; init; }
    public string? UserZak { get; init; }
    public bool UsePublicAppKey { get; init; }
}

public sealed class ZoomOAuthService
{
    private readonly IZoomTokenStore _tokenStore;
    private readonly ZoomOAuthManifest _manifest;
    private readonly HttpClient _http;
    private readonly Func<string, Task> _openUrl;
    private readonly Func<long> _nowSeconds;
    private PendingAuthorization? _pending;

    public ZoomOAuthService(
        IZoomTokenStore tokenStore,
        ZoomOAuthManifest? manifest = null,
        HttpClient? http = null,
        Func<string, Task>? openUrl = null,
        Func<long>? nowSeconds = null)
    {
        _tokenStore = tokenStore;
        _manifest = manifest ?? ZoomOAuthManifest.Load();
        _http = http ?? new HttpClient();
        _openUrl = openUrl ?? DefaultOpenUrlAsync;
        _nowSeconds = nowSeconds ?? (() => DateTimeOffset.UtcNow.ToUnixTimeSeconds());
    }

    public ZoomOAuthManifest Manifest => _manifest;

    public async Task<ZoomOAuthSessionStatus> GetStatusAsync(CancellationToken cancellationToken = default)
    {
        var tokens = await _tokenStore.LoadAsync(cancellationToken).ConfigureAwait(false);
        return new ZoomOAuthSessionStatus
        {
            BrokerConfigured = _manifest.BrokerConfigured,
            SignedIn = !string.IsNullOrWhiteSpace(tokens?.RefreshToken) || !string.IsNullOrWhiteSpace(tokens?.AccessToken),
            ExpiresAt = tokens?.ExpiresAt ?? 0,
            PendingAuthorization = _pending is not null
        };
    }

    public async Task BeginAuthorizationAsync(CancellationToken cancellationToken = default)
    {
        if (!_manifest.BrokerConfigured)
        {
            throw new InvalidOperationException("Zoom OAuth PKCE broker is not configured for this build.");
        }

        if (!Uri.TryCreate(_manifest.BrokerStartUrl, UriKind.Absolute, out var url) ||
            !url.Scheme.StartsWith("http", StringComparison.OrdinalIgnoreCase) ||
            string.IsNullOrWhiteSpace(url.Host))
        {
            throw new InvalidOperationException("The Zoom OAuth broker start URL is not valid.");
        }

        var useBroker = IsBrokerStartUrl(_manifest.BrokerStartUrl);
        var state = ZoomOAuthCrypto.RandomBase64Url(32);
        var verifier = useBroker ? string.Empty : ZoomOAuthCrypto.RandomBase64Url(64);
        var brokerBase = useBroker ? BrokerBaseUrl(_manifest.BrokerStartUrl) : string.Empty;

        var queryParts = new List<KeyValuePair<string, string>>
        {
            new("state", state)
        };
        if (useBroker)
        {
            var brokerCallback = string.IsNullOrWhiteSpace(_manifest.BrokerCallbackUrl)
                ? BrokerEndpoint(BrokerBaseUrl(_manifest.BrokerStartUrl), "/oauth/callback")
                : _manifest.BrokerCallbackUrl;
            queryParts.Add(new("redirect_uri", brokerCallback));
            if (!string.IsNullOrWhiteSpace(_manifest.RedirectUri) &&
                !string.Equals(_manifest.RedirectUri, brokerCallback, StringComparison.OrdinalIgnoreCase))
            {
                queryParts.Add(new("return_uri", _manifest.RedirectUri));
            }
        }
        else
        {
            queryParts.Add(new("response_type", "code"));
            queryParts.Add(new("redirect_uri", _manifest.RedirectUri));
            queryParts.Add(new("scope", _manifest.Scopes));
            queryParts.Add(new("code_challenge", ZoomOAuthCrypto.PkceChallenge(verifier)));
            queryParts.Add(new("code_challenge_method", "S256"));
        }

        var builder = new UriBuilder(url)
        {
            Query = string.Join("&", queryParts.Select(pair =>
                $"{Uri.EscapeDataString(pair.Key)}={Uri.EscapeDataString(pair.Value)}"))
        };
        _pending = new PendingAuthorization(state, verifier, brokerBase);
        await _openUrl(builder.Uri.ToString()).ConfigureAwait(false);
    }

    public async Task HandleRedirectUrlAsync(string callbackUrl, CancellationToken cancellationToken = default)
    {
        if (!Uri.TryCreate(callbackUrl, UriKind.Absolute, out var url))
        {
            throw new InvalidOperationException("OAuth callback URL is not valid.");
        }

        var oauthError = GetQueryValue(url, "error");
        if (!string.IsNullOrWhiteSpace(oauthError))
        {
            throw new InvalidOperationException($"Zoom OAuth failed: {oauthError}");
        }

        var pending = _pending;
        var brokerToken = GetQueryValue(url, "broker_token");
        if (pending is null || (string.IsNullOrWhiteSpace(brokerToken) && string.IsNullOrWhiteSpace(pending.Verifier)))
        {
            throw new InvalidOperationException("Zoom returned a callback but no sign-in is in progress. Start sign-in again.");
        }

        var state = GetQueryValue(url, "state") ?? string.Empty;
        if (string.IsNullOrWhiteSpace(state) || !string.Equals(state, pending.State, StringComparison.Ordinal))
        {
            throw new InvalidOperationException("Zoom OAuth callback state did not match the active sign-in.");
        }

        if (!string.IsNullOrWhiteSpace(brokerToken))
        {
            var redeemUrl = BrokerEndpoint(
                string.IsNullOrWhiteSpace(pending.BrokerBaseUrl)
                    ? BrokerBaseUrl(_manifest.BrokerStartUrl)
                    : pending.BrokerBaseUrl,
                "/oauth/redeem");
            using var content = new StringContent(
                JsonSerializer.Serialize(new { broker_token = brokerToken }),
                Encoding.UTF8,
                "application/json");
            using var response = await _http.PostAsync(redeemUrl, content, cancellationToken).ConfigureAwait(false);
            var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException($"Zoom broker token redeem failed: {OAuthErrorMessage(body, response.ReasonPhrase)}");
            }

            await SaveTokenResponseAsync(body, cancellationToken).ConfigureAwait(false);
            _pending = null;
            return;
        }

        var code = GetQueryValue(url, "code");
        if (string.IsNullOrWhiteSpace(code))
        {
            throw new InvalidOperationException("OAuth callback did not include an authorization code.");
        }

        using (var form = new FormUrlEncodedContent(new Dictionary<string, string>
               {
                   ["grant_type"] = "authorization_code",
                   ["code"] = code,
                   ["redirect_uri"] = _manifest.RedirectUri,
                   ["code_verifier"] = pending.Verifier
               }))
        {
            using var response = await _http.PostAsync("https://zoom.us/oauth/token", form, cancellationToken)
                .ConfigureAwait(false);
            var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                throw new InvalidOperationException($"Zoom token exchange failed: {OAuthErrorMessage(body, response.ReasonPhrase)}");
            }

            await SaveTokenResponseAsync(body, cancellationToken).ConfigureAwait(false);
        }

        _pending = null;
    }

    public async Task SignOutAsync(CancellationToken cancellationToken = default)
    {
        _pending = null;
        await _tokenStore.ClearAsync(cancellationToken).ConfigureAwait(false);
    }

    public async Task<ZoomJoinCredentials> EnsureJoinCredentialsAsync(CancellationToken cancellationToken = default)
    {
        var tokens = await _tokenStore.LoadAsync(cancellationToken).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(tokens?.AccessToken) && string.IsNullOrWhiteSpace(tokens?.RefreshToken))
        {
            return new ZoomJoinCredentials { UsePublicAppKey = true };
        }

        var accessToken = await ResolveValidatedAccessTokenAsync(cancellationToken).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(accessToken))
        {
            return new ZoomJoinCredentials { UsePublicAppKey = true };
        }

        if (_manifest.BrokerConfigured)
        {
            var sdkJwt = await FetchSdkJwtAsync(accessToken, cancellationToken).ConfigureAwait(false);
            var userZak = await FetchZakAsync(accessToken, cancellationToken).ConfigureAwait(false);
            return new ZoomJoinCredentials
            {
                SdkJwt = sdkJwt,
                UserZak = userZak,
                UsePublicAppKey = false
            };
        }

        var zakOnly = await FetchZakAsync(accessToken, cancellationToken).ConfigureAwait(false);
        return new ZoomJoinCredentials { UserZak = zakOnly, UsePublicAppKey = true };
    }

    private async Task<string?> ResolveValidatedAccessTokenAsync(CancellationToken cancellationToken)
    {
        await EnsureAccessTokenAsync(cancellationToken).ConfigureAwait(false);
        var tokens = await _tokenStore.LoadAsync(cancellationToken).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(tokens?.AccessToken))
        {
            return null;
        }

        var validation = await ValidateAccessTokenAsync(tokens.AccessToken, cancellationToken).ConfigureAwait(false);
        if (validation == AccessTokenValidationResult.Valid)
        {
            return tokens.AccessToken;
        }

        if (validation == AccessTokenValidationResult.MissingScopes)
        {
            _pending = null;
            await _tokenStore.ClearAsync(cancellationToken).ConfigureAwait(false);
            throw new InvalidOperationException(
                "Zoom sign-in is missing required permissions (user:read:user). Sign out and sign in again.");
        }

        if (string.IsNullOrWhiteSpace(tokens.RefreshToken))
        {
            throw new InvalidOperationException("Zoom OAuth session expired. Sign in again.");
        }

        await RefreshAccessTokenAsync(tokens.RefreshToken, cancellationToken).ConfigureAwait(false);
        var refreshed = await _tokenStore.LoadAsync(cancellationToken).ConfigureAwait(false);
        if (string.IsNullOrWhiteSpace(refreshed?.AccessToken))
        {
            throw new InvalidOperationException("Zoom OAuth session expired. Sign in again.");
        }

        validation = await ValidateAccessTokenAsync(refreshed.AccessToken, cancellationToken).ConfigureAwait(false);
        if (validation == AccessTokenValidationResult.Valid)
        {
            return refreshed.AccessToken;
        }

        if (validation == AccessTokenValidationResult.MissingScopes)
        {
            _pending = null;
            await _tokenStore.ClearAsync(cancellationToken).ConfigureAwait(false);
            throw new InvalidOperationException(
                "Zoom sign-in is missing required permissions (user:read:user). Sign out and sign in again.");
        }

        throw new InvalidOperationException("Zoom OAuth access token could not be validated. Sign in again.");
    }

    private enum AccessTokenValidationResult
    {
        Valid,
        Unauthorized,
        MissingScopes,
        Failed
    }

    private async Task<AccessTokenValidationResult> ValidateAccessTokenAsync(
        string accessToken,
        CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, "https://api.zoom.us/v2/users/me");
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        using var response = await _http.SendAsync(request, cancellationToken).ConfigureAwait(false);
        if (response.IsSuccessStatusCode)
        {
            return AccessTokenValidationResult.Valid;
        }

        var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        if (IsMissingScopeResponse(body, (int)response.StatusCode))
        {
            return AccessTokenValidationResult.MissingScopes;
        }

        if (response.StatusCode == System.Net.HttpStatusCode.Unauthorized)
        {
            return AccessTokenValidationResult.Unauthorized;
        }

        return AccessTokenValidationResult.Failed;
    }

    private static bool IsMissingScopeResponse(string body, int statusCode)
    {
        if (statusCode != 400 || string.IsNullOrWhiteSpace(body))
        {
            return false;
        }

        try
        {
            using var document = JsonDocument.Parse(body);
            var root = document.RootElement;
            if (root.TryGetProperty("code", out var codeElement) &&
                codeElement.TryGetInt32(out var code) &&
                code == 4711)
            {
                return true;
            }

            if (root.TryGetProperty("message", out var messageElement))
            {
                var message = messageElement.GetString() ?? string.Empty;
                return message.Contains("does not contain scopes", StringComparison.OrdinalIgnoreCase);
            }
        }
        catch
        {
            // ignore
        }

        return body.Contains("does not contain scopes", StringComparison.OrdinalIgnoreCase);
    }

    public static bool IsBrokerStartUrl(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var parsed))
        {
            return false;
        }

        return parsed.Scheme.StartsWith("http", StringComparison.OrdinalIgnoreCase) &&
               parsed.AbsolutePath.TrimEnd('/').Equals("/oauth/start", StringComparison.OrdinalIgnoreCase);
    }

    public static string BrokerBaseUrl(string authorizationUrl)
    {
        var parsed = new Uri(authorizationUrl);
        return $"{parsed.Scheme}://{parsed.Host}";
    }

    public static string BrokerEndpoint(string baseUrl, string path)
    {
        var normalized = baseUrl.EndsWith('/') ? baseUrl : $"{baseUrl}/";
        var endpoint = new Uri(new Uri(normalized), path.TrimStart('/'));
        return endpoint.ToString();
    }

    private async Task EnsureAccessTokenAsync(CancellationToken cancellationToken)
    {
        var tokens = await _tokenStore.LoadAsync(cancellationToken).ConfigureAwait(false)
                     ?? throw new InvalidOperationException("Sign in with Zoom before joining.");
        if (tokens.ExpiresAt > _nowSeconds())
        {
            return;
        }

        if (string.IsNullOrWhiteSpace(tokens.RefreshToken))
        {
            throw new InvalidOperationException("Zoom OAuth session expired. Sign in again.");
        }

        await RefreshAccessTokenAsync(tokens.RefreshToken, cancellationToken).ConfigureAwait(false);
    }

    private async Task RefreshAccessTokenAsync(string refreshToken, CancellationToken cancellationToken)
    {
        if (_manifest.BrokerConfigured)
        {
            var refreshUrl = BrokerEndpoint(BrokerBaseUrl(_manifest.BrokerStartUrl), "/oauth/refresh");
            using var content = new StringContent(
                JsonSerializer.Serialize(new { refresh_token = refreshToken }),
                Encoding.UTF8,
                "application/json");
            using var response = await _http.PostAsync(refreshUrl, content, cancellationToken).ConfigureAwait(false);
            var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                await ClearInvalidGrantAsync(body, cancellationToken).ConfigureAwait(false);
                throw new InvalidOperationException($"Zoom broker token refresh failed: {OAuthErrorMessage(body, response.ReasonPhrase)}");
            }

            await SaveTokenResponseAsync(body, cancellationToken).ConfigureAwait(false);
            return;
        }

        using var form = new FormUrlEncodedContent(new Dictionary<string, string>
        {
            ["grant_type"] = "refresh_token",
            ["refresh_token"] = refreshToken
        });
        using var tokenResponse = await _http.PostAsync("https://zoom.us/oauth/token", form, cancellationToken)
            .ConfigureAwait(false);
        var tokenBody = await tokenResponse.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        if (!tokenResponse.IsSuccessStatusCode)
        {
            await ClearInvalidGrantAsync(tokenBody, cancellationToken).ConfigureAwait(false);
            throw new InvalidOperationException($"Zoom token refresh failed: {OAuthErrorMessage(tokenBody, tokenResponse.ReasonPhrase)}");
        }

        await SaveTokenResponseAsync(tokenBody, cancellationToken).ConfigureAwait(false);
    }

    private async Task ClearInvalidGrantAsync(string body, CancellationToken cancellationToken)
    {
        if (!IsInvalidGrant(body))
        {
            return;
        }

        // A Zoom refresh token is single-use/rotating and can also be revoked.
        // Retrying invalid_grant with the same stored token can never recover and
        // leaves the shell claiming the operator is still signed in. Make the
        // failure terminal and force a clean authorization instead.
        await _tokenStore.ClearAsync(cancellationToken).ConfigureAwait(false);
    }

    private static bool IsInvalidGrant(string body)
    {
        try
        {
            using var document = JsonDocument.Parse(body);
            var root = document.RootElement;
            if (root.TryGetProperty("error", out var errorElement) &&
                string.Equals(errorElement.GetString(), "invalid_grant", StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }

            // The broker may preserve Zoom's response as a string rather than
            // promoting its error field to the top-level payload.
            return root.TryGetProperty("zoom_response", out var responseElement) &&
                   responseElement.GetString()?.Contains("invalid_grant", StringComparison.OrdinalIgnoreCase) == true;
        }
        catch
        {
            return body.Contains("invalid_grant", StringComparison.OrdinalIgnoreCase);
        }
    }

    private async Task<string> FetchZakAsync(string accessToken, CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, "https://api.zoom.us/v2/users/me/token?type=zak");
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        using var response = await _http.SendAsync(request, cancellationToken).ConfigureAwait(false);
        var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"Could not fetch Zoom ZAK: {OAuthErrorMessage(body, response.ReasonPhrase)}");
        }

        using var document = JsonDocument.Parse(body);
        if (!document.RootElement.TryGetProperty("token", out var tokenElement))
        {
            throw new InvalidOperationException("Zoom did not return a ZAK token.");
        }

        return tokenElement.GetString() ?? throw new InvalidOperationException("Zoom did not return a ZAK token.");
    }

    private async Task<string> FetchSdkJwtAsync(string accessToken, CancellationToken cancellationToken)
    {
        var jwtUrl = BrokerEndpoint(BrokerBaseUrl(_manifest.BrokerStartUrl), "/oauth/sdk-jwt");
        using var content = new StringContent(
            JsonSerializer.Serialize(new { access_token = accessToken }),
            Encoding.UTF8,
            "application/json");
        using var response = await _http.PostAsync(jwtUrl, content, cancellationToken).ConfigureAwait(false);
        var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException($"Meeting SDK auth broker failed: {OAuthErrorMessage(body, response.ReasonPhrase)}");
        }

        using var document = JsonDocument.Parse(body);
        if (!document.RootElement.TryGetProperty("sdk_jwt", out var jwtElement))
        {
            throw new InvalidOperationException("Meeting SDK auth broker did not return an SDK JWT.");
        }

        return jwtElement.GetString() ?? throw new InvalidOperationException("Meeting SDK auth broker did not return an SDK JWT.");
    }

    private async Task SaveTokenResponseAsync(string body, CancellationToken cancellationToken)
    {
        using var document = JsonDocument.Parse(body);
        var root = document.RootElement;
        var accessToken = root.TryGetProperty("access_token", out var accessElement) ? accessElement.GetString() : null;
        if (string.IsNullOrWhiteSpace(accessToken))
        {
            var message = root.TryGetProperty("message", out var messageElement) ? messageElement.GetString() : null;
            throw new InvalidOperationException(message ?? "Zoom did not return an access token.");
        }

        var refreshToken = root.TryGetProperty("refresh_token", out var refreshElement) ? refreshElement.GetString() ?? "" : "";
        var expiresIn = root.TryGetProperty("expires_in", out var expiresElement) && expiresElement.TryGetInt32(out var seconds)
            ? seconds
            : 3600;
        if (string.IsNullOrWhiteSpace(refreshToken))
        {
            var existing = await _tokenStore.LoadAsync(cancellationToken).ConfigureAwait(false);
            refreshToken = existing?.RefreshToken ?? "";
        }

        await _tokenStore.SaveAsync(new ZoomOAuthTokens
        {
            AccessToken = accessToken,
            RefreshToken = refreshToken,
            ExpiresAt = _nowSeconds() + expiresIn - 60
        }, cancellationToken).ConfigureAwait(false);
    }

    private static string OAuthErrorMessage(string body, string? fallback)
    {
        try
        {
            using var document = JsonDocument.Parse(body);
            var root = document.RootElement;
            if (root.TryGetProperty("error", out var errorElement) &&
                string.Equals(errorElement.GetString(), "invalid_client", StringComparison.Ordinal))
            {
                return "Zoom rejected the OAuth client. Confirm the Marketplace app is configured for Public Client OAuth (PKCE).";
            }

            if (root.TryGetProperty("error", out var invalidGrantElement) &&
                string.Equals(invalidGrantElement.GetString(), "invalid_grant", StringComparison.OrdinalIgnoreCase))
            {
                return "Zoom sign-in expired or was revoked. Sign in again.";
            }

            if (root.TryGetProperty("reason", out var reasonElement) && !string.IsNullOrWhiteSpace(reasonElement.GetString()))
            {
                return reasonElement.GetString()!;
            }

            if (root.TryGetProperty("error", out var brokerErrorElement) &&
                string.Equals(brokerErrorElement.GetString(), "Zoom OAuth access token could not be validated.", StringComparison.Ordinal) &&
                root.TryGetProperty("zoom_response", out var zoomResponseElement))
            {
                var zoomResponse = zoomResponseElement.GetString() ?? string.Empty;
                if (IsMissingScopeResponse(zoomResponse, 400))
                {
                    return "Zoom sign-in is missing required permissions (user:read:user). Sign out and sign in again.";
                }
            }

            if (root.TryGetProperty("message", out var messageElement) && !string.IsNullOrWhiteSpace(messageElement.GetString()))
            {
                return messageElement.GetString()!;
            }

            if (root.TryGetProperty("error", out var genericError) && !string.IsNullOrWhiteSpace(genericError.GetString()))
            {
                return genericError.GetString()!;
            }
        }
        catch
        {
            // ignore
        }

        return string.IsNullOrWhiteSpace(body) ? fallback ?? "OAuth request failed." : body;
    }

    private static string? GetQueryValue(Uri url, string key)
    {
        var query = url.Query.TrimStart('?');
        if (string.IsNullOrWhiteSpace(query))
        {
            return null;
        }

        foreach (var segment in query.Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var parts = segment.Split('=', 2);
            if (parts.Length == 2 && string.Equals(Uri.UnescapeDataString(parts[0]), key, StringComparison.Ordinal))
            {
                return Uri.UnescapeDataString(parts[1]);
            }
        }

        return null;
    }

    private static Task DefaultOpenUrlAsync(string url)
    {
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
        {
            FileName = url,
            UseShellExecute = true
        });
        return Task.CompletedTask;
    }

    private sealed record PendingAuthorization(string State, string Verifier, string BrokerBaseUrl);
}
