using System.Net;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public enum TelemetrySendStatus
{
    /// <summary>202 — the server accepted the event.</summary>
    Accepted,

    /// <summary>Transient (429 / 5xx / network): may retry later (heartbeat only).</summary>
    RetryLater,

    /// <summary>Permanent for this payload (4xx): do not retry as-is.</summary>
    Rejected
}

public sealed record TelemetrySendResult
{
    public required TelemetrySendStatus Status { get; init; }
    public string? ReportId { get; init; }
    public int? HttpStatus { get; init; }
    public string? Message { get; init; }

    /// <summary>Honored only by the heartbeat path; NEVER at shutdown (spec §S3.4).</summary>
    public TimeSpan? RetryAfter { get; init; }
}

/// <summary>
/// POSTs a telemetry event to the ingest worker's <c>/v1/events</c> per the S0
/// contract: <c>Authorization: Bearer</c>, <c>Content-Type: application/json</c>,
/// body ≤64KB, 202 response. Deliberately the SAME plumbing shape as
/// <see cref="CrashReportUploader"/> (reuses <see cref="CrashReportingConfig"/>
/// for endpoint/key) — one config, one HTTP path. Classifies every outcome so a
/// server error never blocks shutdown and only the heartbeat honors Retry-After.
/// </summary>
public sealed class TelemetryEventClient : IDisposable
{
    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(10);

    private readonly HttpClient _client;

    public TelemetryEventClient(HttpMessageHandler? handler = null, TimeSpan? timeout = null)
    {
        _client = handler is null ? new HttpClient() : new HttpClient(handler, disposeHandler: true);
        _client.Timeout = timeout ?? DefaultTimeout;
    }

    public async Task<TelemetrySendResult> SendAsync(
        Uri endpoint,
        string apiKey,
        string jsonPayload,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(endpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(apiKey);
        ArgumentException.ThrowIfNullOrWhiteSpace(jsonPayload);

        using var request = new HttpRequestMessage(HttpMethod.Post, new Uri(endpoint, "/v1/events"));
        request.Headers.TryAddWithoutValidation("Authorization", $"Bearer {apiKey}");
        request.Content = new StringContent(jsonPayload, Encoding.UTF8);
        request.Content.Headers.ContentType = new System.Net.Http.Headers.MediaTypeHeaderValue("application/json");

        HttpResponseMessage response;
        try
        {
            response = await _client.SendAsync(request, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Shutdown timeout cancelled us: transient, never surfaced.
            return new TelemetrySendResult
            {
                Status = TelemetrySendStatus.RetryLater,
                Message = "Send cancelled (shutdown timeout)"
            };
        }
        catch (Exception ex)
        {
            return new TelemetrySendResult
            {
                Status = TelemetrySendStatus.RetryLater,
                Message = $"Send failed: {ex.Message}"
            };
        }

        using (response)
        {
            var status = (int)response.StatusCode;
            if (response.IsSuccessStatusCode)
            {
                return new TelemetrySendResult
                {
                    Status = TelemetrySendStatus.Accepted,
                    HttpStatus = status,
                    ReportId = await TryReadReportIdAsync(response, cancellationToken).ConfigureAwait(false)
                };
            }

            var body = await SafeReadBodyAsync(response, cancellationToken).ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.TooManyRequests || status >= 500)
            {
                return new TelemetrySendResult
                {
                    Status = TelemetrySendStatus.RetryLater,
                    HttpStatus = status,
                    RetryAfter = ReadRetryAfter(response),
                    Message = $"Server busy or unavailable (HTTP {status}): {body}"
                };
            }

            return new TelemetrySendResult
            {
                Status = TelemetrySendStatus.Rejected,
                HttpStatus = status,
                Message = $"Server rejected the event (HTTP {status}): {body}"
            };
        }
    }

    public void Dispose() => _client.Dispose();

    private static TimeSpan? ReadRetryAfter(HttpResponseMessage response)
    {
        var retryAfter = response.Headers.RetryAfter;
        if (retryAfter?.Delta is { } delta)
        {
            return delta;
        }

        if (retryAfter?.Date is { } date)
        {
            var wait = date - DateTimeOffset.UtcNow;
            return wait > TimeSpan.Zero ? wait : TimeSpan.Zero;
        }

        return null;
    }

    private static async Task<string?> TryReadReportIdAsync(
        HttpResponseMessage response, CancellationToken cancellationToken)
    {
        try
        {
            var json = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            using var doc = JsonDocument.Parse(json);
            return doc.RootElement.TryGetProperty("reportId", out var id) && id.ValueKind == JsonValueKind.String
                ? id.GetString()
                : null;
        }
        catch
        {
            return null;
        }
    }

    private static async Task<string> SafeReadBodyAsync(
        HttpResponseMessage response, CancellationToken cancellationToken)
    {
        try
        {
            var text = (await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false)).Trim();
            return text.Length > 300 ? text[..300] : text;
        }
        catch
        {
            return string.Empty;
        }
    }
}
