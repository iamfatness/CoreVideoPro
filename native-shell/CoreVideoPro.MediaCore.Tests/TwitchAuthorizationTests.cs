using System.Net;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class TwitchAuthorizationTests
{
    [Fact]
    public async Task OversizedProviderResponseIsSanitizedAndFailsClosed()
    {
        using var transport = new TwitchHttpTransport((_, _) => Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) {
            Content = new StringContent(new string('x', 1024 * 1024 + 1)) }));
        var error = await Assert.ThrowsAsync<TwitchChatException>(() => transport.ValidateAsync(Tokens(new Clock()), CancellationToken.None));
        Assert.Equal(TwitchChatFailure.Invalid, error.Failure);
    }
    internal const string Client = "fixtureclient12345678";
    internal sealed class Clock : TimeProvider
    {
        public DateTimeOffset Now = new(2026, 11, 5, 6, 0, 0, TimeSpan.Zero);
        public override DateTimeOffset GetUtcNow() => Now;
        public override long GetTimestamp() => Now.UtcTicks;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;
        public Task Delay(TimeSpan span, CancellationToken cancellation) { cancellation.ThrowIfCancellationRequested(); Now += span; return Task.CompletedTask; }
    }
    internal static TwitchCredentials Tokens(Clock clock) => new(Client, "fixture-access-token", "fixture-refresh-token",
        clock.Now.AddHours(4), "5678", "fixtureviewer");
    internal static HttpResponseMessage Json(string body, HttpStatusCode status = HttpStatusCode.OK) => new(status) { Content = new StringContent(body) };
    internal static string Validation(string user = "5678", string client = Client, int expires = 14400) => JsonSerializer.Serialize(new
    { client_id = client, user_id = user, login = "fixtureviewer", scopes = new[] { "user:read:chat" }, expires_in = expires });

    [Fact]
    public async Task PublicDeviceFlowUsesReadChatOnlyAndStoresOnlyValidatedUserTokens()
    {
        var clock = new Clock();
        TwitchCredentials? stored = null;
        var prompts = new List<TwitchDevicePrompt>();
        var polls = 0;
        var transport = new TwitchHttpTransport(async (request, cancellation) =>
        {
            Assert.Equal("id.twitch.tv", request.RequestUri!.Host);
            var body = request.Content is null ? "" : await request.Content.ReadAsStringAsync(cancellation);
            Assert.DoesNotContain("client_secret", body);
            if (request.RequestUri.AbsolutePath == "/oauth2/device")
            {
                Assert.Contains("scopes=user%3Aread%3Achat", body);
                return Json("""{"device_code":"private-fixture-device","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":1800,"interval":5}""");
            }
            if (request.RequestUri.AbsolutePath == "/oauth2/token")
            {
                Assert.Contains("device_code=private-fixture-device", body);
                if (++polls == 1) return Json("""{"message":"authorization_pending"}""", HttpStatusCode.BadRequest);
                return Json("""{"access_token":"fixture-access-token","refresh_token":"fixture-refresh-token","expires_in":14400,"token_type":"bearer","scope":["user:read:chat"]}""");
            }
            Assert.Equal("/oauth2/validate", request.RequestUri.AbsolutePath);
            Assert.Null(stored);
            return Json(Validation());
        });
        using var auth = new TwitchAuthorization(transport, () => stored, value => stored = value, () => stored = null, clock, clock.Delay);
        var result = await auth.AuthorizeAsync(Client, prompts.Add, CancellationToken.None);
        Assert.Equal("5678", result.UserId);
        Assert.Equal(result, stored);
        Assert.Equal("ABCDEFGH", Assert.Single(prompts).UserCode);
        Assert.DoesNotContain("fixture-access-token", stored!.ToString());
        Assert.Equal(2, polls);
    }

    [Theory]
    [InlineData("access_denied")]
    [InlineData("expired_token")]
    public async Task DeniedOrExpiredConsentNeverStoresCredentials(string failure)
    {
        var clock = new Clock();
        var transport = new TwitchHttpTransport((request, _) => Task.FromResult(request.RequestUri!.AbsolutePath.EndsWith("device")
            ? Json("""{"device_code":"fixture-device-code","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":20,"interval":5}""")
            : Json(JsonSerializer.Serialize(new { message = failure }), HttpStatusCode.BadRequest)));
        using var auth = new TwitchAuthorization(transport, () => null, _ => Assert.Fail("Stored unapproved credentials"), () => { }, clock, clock.Delay);
        await Assert.ThrowsAsync<TwitchChatException>(() => auth.AuthorizeAsync(Client, _ => { }, CancellationToken.None));
    }

    [Fact]
    public async Task DeviceDeadlineExpiresWithoutRepeatingTheDeviceGrant()
    {
        var clock = new Clock();
        var starts = 0;
        var transport = new TwitchHttpTransport((request, _) =>
        {
            if (request.RequestUri!.AbsolutePath.EndsWith("device"))
            {
                ++starts;
                return Task.FromResult(Json("""{"device_code":"fixture-device-code","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":9,"interval":5}"""));
            }
            return Task.FromResult(Json("""{"message":"authorization_pending"}""", HttpStatusCode.BadRequest));
        });
        using var auth = new TwitchAuthorization(transport, () => null, _ => Assert.Fail("Stored expired credentials"), () => { }, clock, clock.Delay);
        await Assert.ThrowsAsync<TwitchChatException>(() => auth.AuthorizeAsync(Client, _ => { }, CancellationToken.None));
        Assert.Equal(1, starts);
    }

    [Fact]
    public async Task HourlyValidationAndSingleUseRefreshAreSerializedAndIdentityCannotChange()
    {
        var clock = new Clock();
        TwitchCredentials? stored = Tokens(clock);
        var refreshes = 0;
        var validations = 0;
        var foreign = false;
        var issued = clock.Now;
        var transport = new TwitchHttpTransport(async (request, cancellation) =>
        {
            if (request.RequestUri!.AbsolutePath == "/oauth2/token")
            {
                Interlocked.Increment(ref refreshes);
                issued = clock.Now;
                Assert.Contains("fixture-refresh-token", await request.Content!.ReadAsStringAsync(cancellation));
                return Json("""{"access_token":"rotated-access-token","refresh_token":"rotated-refresh-token","expires_in":14400,"token_type":"bearer","scope":["user:read:chat"]}""");
            }
            Interlocked.Increment(ref validations);
            return Json(Validation(foreign ? "9988" : "5678", expires: (int)(issued.AddHours(4) - clock.Now).TotalSeconds));
        });
        using var auth = new TwitchAuthorization(transport, () => stored, value => stored = value, () => stored = null, clock, clock.Delay);
        await auth.AuthorizeAsync(Client, _ => Assert.Fail("Unexpected device prompt"), CancellationToken.None);
        clock.Now += TimeSpan.FromHours(1);
        await auth.MaintainAsync(CancellationToken.None);
        Assert.Equal(2, validations);
        clock.Now += TimeSpan.FromHours(3);
        await Task.WhenAll(Enumerable.Range(0, 8).Select(_ => auth.MaintainAsync(CancellationToken.None)));
        Assert.Equal(1, refreshes);
        Assert.Equal("rotated-refresh-token", stored!.RefreshToken);
        foreign = true;
        clock.Now += TimeSpan.FromHours(1);
        await Assert.ThrowsAsync<TwitchChatException>(() => auth.MaintainAsync(CancellationToken.None));
        Assert.Null(stored);
    }
}
