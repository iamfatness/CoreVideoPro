using System.Globalization;
using System.Net;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Telemetry-ingest endpoint configuration. Both keys empty by default =
/// crash reporting quietly disabled (dev builds); the beta config ships them.
/// </summary>
public sealed class CrashReportingConfig
{
    public const string EndpointEnvVar = "COREVIDEO_TELEMETRY_ENDPOINT";
    public const string ApiKeyEnvVar = "COREVIDEO_TELEMETRY_API_KEY";

    public string? Endpoint { get; init; }
    public string? ApiKey { get; init; }

    public bool Enabled =>
        !string.IsNullOrWhiteSpace(ApiKey) &&
        Uri.TryCreate(Endpoint, UriKind.Absolute, out var uri) &&
        (uri.Scheme == Uri.UriSchemeHttps || uri.Scheme == Uri.UriSchemeHttp);

    public static CrashReportingConfig FromEnvironment() => new()
    {
        Endpoint = Environment.GetEnvironmentVariable(EndpointEnvVar)?.Trim(),
        ApiKey = Environment.GetEnvironmentVariable(ApiKeyEnvVar)?.Trim()
    };
}

public enum CrashReportUploadStatus
{
    /// <summary>202 — the server issued a reportId.</summary>
    Accepted,

    /// <summary>Transient (429 / 5xx / network): keep the zip, try again later.</summary>
    RetryLater,

    /// <summary>Permanent for this payload (400/401/413/415): do NOT retry the same zip.</summary>
    Rejected
}

public sealed class CrashReportUploadResult
{
    public required CrashReportUploadStatus Status { get; init; }
    public string? ReportId { get; init; }
    public int? HttpStatus { get; init; }
    public string? Message { get; init; }
    public TimeSpan? RetryAfter { get; init; }
}

public sealed class CrashReportUploadOptions
{
    /// <summary>Base endpoint, e.g. <c>https://corevideo-telemetry-ingest.example.workers.dev</c>.</summary>
    public required Uri Endpoint { get; init; }
    public required string ApiKey { get; init; }
    public string? Version { get; init; }
    public string? MachineClass { get; init; }
    public string? Reason { get; init; }
}

/// <summary>
/// POSTs a crash-report zip to the telemetry-ingest worker per the S0/S1
/// contract: <c>POST /v1/crashes</c>, <c>Authorization: Bearer</c>,
/// <c>Content-Type: application/zip</c>, metadata in <c>X-CoreVideo-*</c>
/// headers. Classifies every outcome so the caller never retries a payload the
/// server permanently rejected and never blocks the app on a server error.
/// </summary>
public sealed class CrashReportUploader : IDisposable
{
    private static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(120); // 24MB on venue Wi-Fi

    private readonly HttpClient _client;

    /// <summary><paramref name="handler"/> is injectable for tests (fake server).</summary>
    public CrashReportUploader(HttpMessageHandler? handler = null, TimeSpan? timeout = null)
    {
        _client = handler is null ? new HttpClient() : new HttpClient(handler, disposeHandler: true);
        _client.Timeout = timeout ?? DefaultTimeout;
    }

    public async Task<CrashReportUploadResult> UploadAsync(
        CrashReportUploadOptions options,
        string zipPath,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentException.ThrowIfNullOrWhiteSpace(zipPath);

        byte[] zipBytes;
        try
        {
            zipBytes = await File.ReadAllBytesAsync(zipPath, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return new CrashReportUploadResult
            {
                Status = CrashReportUploadStatus.Rejected,
                Message = $"Could not read the report archive: {ex.Message}"
            };
        }

        using var request = new HttpRequestMessage(HttpMethod.Post, new Uri(options.Endpoint, "/v1/crashes"));
        request.Headers.TryAddWithoutValidation("Authorization", $"Bearer {options.ApiKey}");
        AddMetadataHeader(request, "X-CoreVideo-Version", options.Version);
        AddMetadataHeader(request, "X-CoreVideo-Machine-Class", options.MachineClass);
        AddMetadataHeader(request, "X-CoreVideo-Reason", options.Reason);
        request.Content = new ByteArrayContent(zipBytes);
        request.Content.Headers.TryAddWithoutValidation("Content-Type", "application/zip");

        HttpResponseMessage response;
        try
        {
            response = await _client.SendAsync(request, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex)
        {
            // Timeout / DNS / offline venue network: transient by definition.
            return new CrashReportUploadResult
            {
                Status = CrashReportUploadStatus.RetryLater,
                Message = $"Upload failed: {ex.Message}"
            };
        }

        using (response)
        {
            var status = (int)response.StatusCode;
            if (response.IsSuccessStatusCode)
            {
                var reportId = await TryReadReportIdAsync(response, cancellationToken).ConfigureAwait(false);
                return new CrashReportUploadResult
                {
                    Status = CrashReportUploadStatus.Accepted,
                    HttpStatus = status,
                    ReportId = reportId,
                    Message = reportId is null ? "Accepted (no reportId in response)" : null
                };
            }

            var body = await SafeReadBodyAsync(response, cancellationToken).ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.TooManyRequests || status >= 500)
            {
                return new CrashReportUploadResult
                {
                    Status = CrashReportUploadStatus.RetryLater,
                    HttpStatus = status,
                    RetryAfter = ReadRetryAfter(response),
                    Message = $"Server busy or unavailable (HTTP {status}): {body}"
                };
            }

            // 400 / 401 / 413 / 415 and anything else in 4xx: this payload will
            // never succeed as-is — keep it on disk but do not auto-retry.
            return new CrashReportUploadResult
            {
                Status = CrashReportUploadStatus.Rejected,
                HttpStatus = status,
                Message = $"Server rejected the report (HTTP {status}): {body}"
            };
        }
    }

    public void Dispose() => _client.Dispose();

    private static void AddMetadataHeader(HttpRequestMessage request, string name, string? value)
    {
        if (!string.IsNullOrWhiteSpace(value))
        {
            request.Headers.TryAddWithoutValidation(name, value.Trim());
        }
    }

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
            var text = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            text = text.Trim();
            return text.Length > 300 ? text[..300] : text;
        }
        catch
        {
            return string.Empty;
        }
    }
}
