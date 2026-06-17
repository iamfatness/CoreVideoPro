using System.Net;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomOAuthServiceTests
{
    [Fact]
    public async Task GetStatus_ReportsBrokerConfiguredFromManifest()
    {
        var store = new MemoryZoomTokenStore();
        var service = new ZoomOAuthService(store, new ZoomOAuthManifest
        {
            BrokerStartUrl = "https://corevideo.iamfatness.us/oauth/start",
            BrokerCallbackUrl = "https://corevideo.iamfatness.us/oauth/callback",
            RedirectUri = "corevideo://oauth/callback"
        });

        var status = await service.GetStatusAsync();

        Assert.True(status.BrokerConfigured);
        Assert.False(status.SignedIn);
    }

    [Fact]
    public async Task HandleRedirectUrl_RedeemsBrokerTokenAndStoresTokens()
    {
        var store = new MemoryZoomTokenStore();
        var handler = new StubHttpHandler(request =>
        {
            if (request.RequestUri?.AbsolutePath.EndsWith("/oauth/redeem", StringComparison.Ordinal) == true)
            {
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent(
                        """{"access_token":"access-1","refresh_token":"refresh-1","expires_in":3600}""",
                        Encoding.UTF8,
                        "application/json")
                };
            }

            throw new InvalidOperationException($"Unexpected request {request.RequestUri}");
        });

        var openUrl = string.Empty;
        var service2 = new ZoomOAuthService(
            store,
            new ZoomOAuthManifest
            {
                BrokerStartUrl = "https://corevideo.iamfatness.us/oauth/start",
                BrokerCallbackUrl = "https://corevideo.iamfatness.us/oauth/callback",
                RedirectUri = "corevideo://oauth/callback"
            },
            new HttpClient(handler),
            url =>
            {
                openUrl = url;
                return Task.CompletedTask;
            },
            () => 1_000);
        await service2.BeginAuthorizationAsync();
        Assert.False(string.IsNullOrWhiteSpace(openUrl));
        Assert.Contains("redirect_uri=https%3A%2F%2Fcorevideo.iamfatness.us%2Foauth%2Fcallback", openUrl, StringComparison.Ordinal);
        Assert.Contains("return_uri=corevideo%3A%2F%2Foauth%2Fcallback", openUrl, StringComparison.Ordinal);
        var query = new Uri(openUrl).Query.TrimStart('?');
        var capturedState = query.Split('&', StringSplitOptions.RemoveEmptyEntries)
            .Select(part => part.Split('=', 2))
            .Where(parts => parts.Length == 2 && parts[0] == "state")
            .Select(parts => Uri.UnescapeDataString(parts[1]))
            .First();

        await service2.HandleRedirectUrlAsync(
            $"corevideo://oauth/callback?state={Uri.EscapeDataString(capturedState)}&broker_token=broker-1");

        var saved = await store.LoadAsync();
        Assert.Equal("access-1", saved?.AccessToken);
        Assert.Equal("refresh-1", saved?.RefreshToken);
        Assert.True((await service2.GetStatusAsync()).SignedIn);
    }

    [Fact]
    public async Task EnsureJoinCredentials_ThreadsBrokerSdkJwtAndZak()
    {
        var store = new MemoryZoomTokenStore();
        await store.SaveAsync(new ZoomOAuthTokens
        {
            AccessToken = "access-1",
            RefreshToken = "refresh-1",
            ExpiresAt = 9_999
        });

        var handler = new StubHttpHandler(request =>
        {
            if (request.RequestUri?.AbsolutePath.EndsWith("/oauth/sdk-jwt", StringComparison.Ordinal) == true)
            {
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent("""{"sdk_jwt":"sdk-jwt-1"}""", Encoding.UTF8, "application/json")
                };
            }

            if (request.RequestUri?.AbsolutePath.Contains("/users/me/token", StringComparison.Ordinal) == true)
            {
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent("""{"token":"zak-1"}""", Encoding.UTF8, "application/json")
                };
            }

            throw new InvalidOperationException($"Unexpected request {request.RequestUri}");
        });

        var service = new ZoomOAuthService(
            store,
            new ZoomOAuthManifest
            {
                BrokerStartUrl = "https://corevideo.iamfatness.us/oauth/start",
                BrokerCallbackUrl = "https://corevideo.iamfatness.us/oauth/callback",
                RedirectUri = "corevideo://oauth/callback"
            },
            new HttpClient(handler),
            nowSeconds: () => 1_000);

        var creds = await service.EnsureJoinCredentialsAsync();

        Assert.Equal("sdk-jwt-1", creds.SdkJwt);
        Assert.Equal("zak-1", creds.UserZak);
        Assert.False(creds.UsePublicAppKey);
    }

    private sealed class MemoryZoomTokenStore : IZoomTokenStore
    {
        private ZoomOAuthTokens? _tokens;

        public Task<ZoomOAuthTokens?> LoadAsync(CancellationToken cancellationToken = default) =>
            Task.FromResult(_tokens);

        public Task SaveAsync(ZoomOAuthTokens tokens, CancellationToken cancellationToken = default)
        {
            _tokens = tokens;
            return Task.CompletedTask;
        }

        public Task ClearAsync(CancellationToken cancellationToken = default)
        {
            _tokens = null;
            return Task.CompletedTask;
        }
    }

    private sealed class StubHttpHandler : HttpMessageHandler
    {
        private readonly Func<HttpRequestMessage, HttpResponseMessage> _handler;

        public StubHttpHandler(Func<HttpRequestMessage, HttpResponseMessage> handler) => _handler = handler;

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request,
            CancellationToken cancellationToken) =>
            Task.FromResult(_handler(request));
    }
}