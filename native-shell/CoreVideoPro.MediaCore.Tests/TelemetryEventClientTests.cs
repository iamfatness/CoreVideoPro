using System.Net;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// The /v1/events client against a fake handler: the S0 contract (Bearer auth,
/// application/json, POST to /v1/events), 202 reportId parsing, and the retry
/// classification (429 → RetryLater + Retry-After; 4xx → Rejected; 5xx/network →
/// RetryLater).
/// </summary>
public sealed class TelemetryEventClientTests
{
    private sealed class FakeHandler(Func<HttpRequestMessage, string, HttpResponseMessage> respond)
        : HttpMessageHandler
    {
        public HttpRequestMessage? LastRequest;
        public string? LastBody;

        protected override async Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
        {
            LastRequest = request;
            LastBody = request.Content is null ? "" : await request.Content.ReadAsStringAsync(cancellationToken);
            return respond(request, LastBody);
        }
    }

    private static readonly Uri Endpoint = new("https://telemetry.example");

    [Fact]
    public async Task Send_PostsTheContractRequest_AndParsesReportId()
    {
        var handler = new FakeHandler((_, _) => new HttpResponseMessage((HttpStatusCode)202)
        {
            Content = new StringContent("{\"reportId\":\"cv-20260723-abc\",\"accepted\":true}")
        });
        using var client = new TelemetryEventClient(handler);

        var result = await client.SendAsync(Endpoint, "test-key", "{\"name\":\"session-end\"}");

        Assert.Equal(TelemetrySendStatus.Accepted, result.Status);
        Assert.Equal("cv-20260723-abc", result.ReportId);
        Assert.Equal(HttpMethod.Post, handler.LastRequest!.Method);
        Assert.Equal("/v1/events", handler.LastRequest.RequestUri!.AbsolutePath);
        Assert.Equal("Bearer test-key", handler.LastRequest.Headers.Authorization?.ToString());
        Assert.Equal("application/json", handler.LastRequest.Content!.Headers.ContentType!.MediaType);
        Assert.Equal("{\"name\":\"session-end\"}", handler.LastBody);
    }

    [Fact]
    public async Task Send_TooManyRequests_IsRetryLater_WithRetryAfter()
    {
        var response = new HttpResponseMessage(HttpStatusCode.TooManyRequests);
        response.Headers.RetryAfter = new System.Net.Http.Headers.RetryConditionHeaderValue(TimeSpan.FromSeconds(30));
        var handler = new FakeHandler((_, _) => response);
        using var client = new TelemetryEventClient(handler);

        var result = await client.SendAsync(Endpoint, "k", "{}");

        Assert.Equal(TelemetrySendStatus.RetryLater, result.Status);
        Assert.Equal(TimeSpan.FromSeconds(30), result.RetryAfter);
    }

    [Fact]
    public async Task Send_ServerError_IsRetryLater()
    {
        var handler = new FakeHandler((_, _) => new HttpResponseMessage(HttpStatusCode.InternalServerError));
        using var client = new TelemetryEventClient(handler);

        var result = await client.SendAsync(Endpoint, "k", "{}");
        Assert.Equal(TelemetrySendStatus.RetryLater, result.Status);
    }

    [Fact]
    public async Task Send_BadRequest_IsRejected_NotRetried()
    {
        var handler = new FakeHandler((_, _) => new HttpResponseMessage(HttpStatusCode.BadRequest));
        using var client = new TelemetryEventClient(handler);

        var result = await client.SendAsync(Endpoint, "k", "{}");
        Assert.Equal(TelemetrySendStatus.Rejected, result.Status);
    }
}
