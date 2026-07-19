using System.Net;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// S1 upload client against a fake HTTP handler: contract headers, 202
/// reportId parsing, and the retry classification (413 = never retry the same
/// payload; 429 honors Retry-After; 5xx/network = retry later).
/// </summary>
public sealed class CrashReportUploaderTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cvp-upload-").FullName;
    private readonly string _zipPath;

    public CrashReportUploaderTests()
    {
        _zipPath = Path.Combine(_dir, "report.zip");
        File.WriteAllBytes(_zipPath, [0x50, 0x4b, 0x03, 0x04, 0x42]);
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(_dir, recursive: true);
        }
        catch
        {
            // best effort
        }
    }

    private sealed class FakeHandler : HttpMessageHandler
    {
        private readonly Func<HttpRequestMessage, byte[], HttpResponseMessage> _respond;

        public HttpRequestMessage? LastRequest;
        public byte[]? LastBody;

        public FakeHandler(Func<HttpRequestMessage, byte[], HttpResponseMessage> respond) =>
            _respond = respond;

        protected override async Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken)
        {
            LastRequest = request;
            LastBody = request.Content is null
                ? []
                : await request.Content.ReadAsByteArrayAsync(cancellationToken);
            return _respond(request, LastBody);
        }
    }

    private static CrashReportUploadOptions Options() => new()
    {
        Endpoint = new Uri("https://telemetry.example"),
        ApiKey = "test-key",
        Version = "0.1.0",
        MachineClass = "win-x64-cpu32-ram64gb",
        Reason = "wer-dump: corevideo-native.exe"
    };

    [Fact]
    public async Task Upload_SendsTheContractRequest_AndParsesTheReportId()
    {
        var handler = new FakeHandler((_, _) => new HttpResponseMessage((HttpStatusCode)202)
        {
            Content = new StringContent("{\"reportId\":\"cv-20260718-abc\",\"accepted\":true}")
        });
        using var uploader = new CrashReportUploader(handler);

        var result = await uploader.UploadAsync(Options(), _zipPath);

        Assert.Equal(CrashReportUploadStatus.Accepted, result.Status);
        Assert.Equal("cv-20260718-abc", result.ReportId);
        Assert.Equal(202, result.HttpStatus);

        var request = Assert.IsType<HttpRequestMessage>(handler.LastRequest);
        Assert.Equal(HttpMethod.Post, request.Method);
        Assert.Equal("https://telemetry.example/v1/crashes", request.RequestUri!.ToString());
        Assert.Equal("Bearer test-key", request.Headers.GetValues("Authorization").Single());
        Assert.Equal("0.1.0", request.Headers.GetValues("X-CoreVideo-Version").Single());
        Assert.Equal("win-x64-cpu32-ram64gb", request.Headers.GetValues("X-CoreVideo-Machine-Class").Single());
        Assert.Equal("wer-dump: corevideo-native.exe", request.Headers.GetValues("X-CoreVideo-Reason").Single());
        Assert.Equal("application/zip", request.Content!.Headers.GetValues("Content-Type").Single());
        Assert.Equal(File.ReadAllBytes(_zipPath), handler.LastBody);
    }

    [Fact]
    public async Task Upload_413_IsRejected_NeverRetrySamePayload()
    {
        var handler = new FakeHandler((_, _) => new HttpResponseMessage(HttpStatusCode.RequestEntityTooLarge)
        {
            Content = new StringContent("Payload too large (max 26214400 bytes)")
        });
        using var uploader = new CrashReportUploader(handler);

        var result = await uploader.UploadAsync(Options(), _zipPath);

        Assert.Equal(CrashReportUploadStatus.Rejected, result.Status);
        Assert.Equal(413, result.HttpStatus);
        Assert.Contains("413", result.Message);
    }

    [Fact]
    public async Task Upload_429_IsRetryLater_HonoringRetryAfter()
    {
        var handler = new FakeHandler((_, _) =>
        {
            var response = new HttpResponseMessage(HttpStatusCode.TooManyRequests)
            {
                Content = new StringContent("Too many requests")
            };
            response.Headers.TryAddWithoutValidation("Retry-After", "60");
            return response;
        });
        using var uploader = new CrashReportUploader(handler);

        var result = await uploader.UploadAsync(Options(), _zipPath);

        Assert.Equal(CrashReportUploadStatus.RetryLater, result.Status);
        Assert.Equal(429, result.HttpStatus);
        Assert.Equal(TimeSpan.FromSeconds(60), result.RetryAfter);
    }

    [Fact]
    public async Task Upload_500_IsRetryLater_NeverThrows()
    {
        var handler = new FakeHandler((_, _) => new HttpResponseMessage(HttpStatusCode.InternalServerError)
        {
            Content = new StringContent("boom")
        });
        using var uploader = new CrashReportUploader(handler);

        var result = await uploader.UploadAsync(Options(), _zipPath);

        Assert.Equal(CrashReportUploadStatus.RetryLater, result.Status);
        Assert.Equal(500, result.HttpStatus);
    }

    [Fact]
    public async Task Upload_NetworkFailure_IsRetryLater_NeverThrows()
    {
        var handler = new FakeHandler((_, _) => throw new HttpRequestException("connection refused"));
        using var uploader = new CrashReportUploader(handler);

        var result = await uploader.UploadAsync(Options(), _zipPath);

        Assert.Equal(CrashReportUploadStatus.RetryLater, result.Status);
        Assert.Null(result.HttpStatus);
        Assert.Contains("connection refused", result.Message);
    }

    [Fact]
    public async Task Upload_MissingZipFile_IsRejectedWithAClearMessage()
    {
        using var uploader = new CrashReportUploader(new FakeHandler(
            (_, _) => new HttpResponseMessage((HttpStatusCode)202)));

        var result = await uploader.UploadAsync(Options(), Path.Combine(_dir, "missing.zip"));

        Assert.Equal(CrashReportUploadStatus.Rejected, result.Status);
        Assert.Contains("Could not read the report archive", result.Message);
    }

    [Fact]
    public void Config_EmptyDefaults_DisableTheFeatureQuietly()
    {
        Assert.False(new CrashReportingConfig().Enabled);
        Assert.False(new CrashReportingConfig { Endpoint = "https://x.example" }.Enabled);
        Assert.False(new CrashReportingConfig { ApiKey = "k" }.Enabled);
        Assert.False(new CrashReportingConfig { Endpoint = "not-a-url", ApiKey = "k" }.Enabled);
        Assert.True(new CrashReportingConfig { Endpoint = "https://x.example", ApiKey = "k" }.Enabled);
    }
}
