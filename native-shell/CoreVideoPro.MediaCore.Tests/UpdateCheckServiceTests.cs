using System.Net;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class AppUpdateVersionTests
{
    [Theory]
    [InlineData("1.2.3", true)]
    [InlineData("v1.2.3", true)]
    [InlineData("1.2.3.0", true)]
    [InlineData("0.1.0", true)]
    [InlineData("1.2", true)]
    [InlineData("", false)]
    [InlineData(null, false)]
    [InlineData("1", false)]
    [InlineData("1.2.3.4.5", false)]
    [InlineData("1.two.3", false)]
    [InlineData("1.-2.3", false)]
    [InlineData("latest", false)]
    public void TryParse_AcceptsOnlyVersionShapes(string? text, bool expected) =>
        Assert.Equal(expected, AppUpdateVersion.TryParse(text, out _));

    [Theory]
    [InlineData("1.2.4", "1.2.3", true)]
    [InlineData("2.0.0", "1.9.9", true)]
    [InlineData("1.10.0", "1.9.9", true)]  // numeric, not string, comparison
    [InlineData("0.2.0", "0.1.0", true)]
    [InlineData("1.2.3", "1.2.3", false)]
    [InlineData("1.2.3.0", "1.2.3", false)] // x.y.z.0 == x.y.z
    [InlineData("1.2.2", "1.2.3", false)]
    [InlineData("0.9.9", "1.0.0", false)]
    [InlineData("not-a-version", "1.0.0", false)]
    [InlineData("1.0.1", "garbage", false)]
    public void IsNewer_ComparesNumerically(string candidate, string current, bool expected) =>
        Assert.Equal(expected, AppUpdateVersion.IsNewer(candidate, current));

    [Theory]
    [InlineData("1.2.3", "1.2.3", true)]
    [InlineData("v1.2.3", "1.2.3.0", true)]
    [InlineData("1.2.3", "1.2.4", false)]
    [InlineData("garbage", "garbage", false)]
    public void AreEqual_NormalizesBeforeComparing(string left, string right, bool expected) =>
        Assert.Equal(expected, AppUpdateVersion.AreEqual(left, right));
}

public sealed class UpdateFeedParserTests
{
    [Fact]
    public void Parses_TheFullShape_MakeAppinstallerEmits()
    {
        var entry = UpdateFeedParser.TryParse(
            """
            {
              "version": "1.2.3",
              "msixUrl": "https://updates.example.com/CoreVideoPro-1.2.3.msix",
              "appinstallerUrl": "https://updates.example.com/CoreVideoPro.appinstaller",
              "sha256": "abc123"
            }
            """);

        Assert.NotNull(entry);
        Assert.Equal("1.2.3", entry.Version);
        Assert.Equal("https://updates.example.com/CoreVideoPro-1.2.3.msix", entry.MsixUrl);
        Assert.Equal("https://updates.example.com/CoreVideoPro.appinstaller", entry.AppInstallerUrl);
        Assert.Equal("abc123", entry.Sha256);
        Assert.Equal("https://updates.example.com/CoreVideoPro.appinstaller", entry.DownloadUrl);
    }

    [Fact]
    public void Parses_MinimalShape_AndPrefersMsixUrlWhenNoAppinstaller()
    {
        var entry = UpdateFeedParser.TryParse("""{"version":"2.0.0","msixUrl":"https://x/a.msix"}""");
        Assert.NotNull(entry);
        Assert.Equal("https://x/a.msix", entry.DownloadUrl);
        Assert.Null(entry.Sha256);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("not json at all")]
    [InlineData("[]")]
    [InlineData("{}")]
    [InlineData("""{"version":"latest"}""")]
    [InlineData("""{"version":42}""")]
    public void Malformed_YieldsNull_NeverThrows(string? json) =>
        Assert.Null(UpdateFeedParser.TryParse(json));
}

public sealed class DismissedUpdateVersionStoreTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cvp-update-dismissed-").FullName;

    private string StorePath => Path.Combine(_dir, "nested", "update-dismissed.json");

    public void Dispose() => Directory.Delete(_dir, recursive: true);

    [Fact]
    public void RoundTrips_AndSuppressesOnlyTheDismissedVersion()
    {
        var store = new DismissedUpdateVersionStore(StorePath);
        Assert.Null(store.Load());
        Assert.False(store.IsDismissed("1.2.3"));

        store.Save("1.2.3");
        Assert.Equal("1.2.3", store.Load());
        Assert.True(store.IsDismissed("1.2.3"));
        Assert.True(store.IsDismissed("v1.2.3.0")); // normalized compare, not string compare
        Assert.False(store.IsDismissed("1.2.4"));   // a NEWER release shows the banner again
    }

    [Fact]
    public void MalformedOrGarbageFile_ReadsAsNotDismissed()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(StorePath)!);
        File.WriteAllText(StorePath, "{ this is not json");
        var store = new DismissedUpdateVersionStore(StorePath);
        Assert.Null(store.Load());
        Assert.False(store.IsDismissed("1.2.3"));

        File.WriteAllText(StorePath, """{"dismissedVersion":"banana"}""");
        Assert.Null(store.Load());
    }
}

public sealed class UpdateCheckServiceTests
{
    private sealed class StubHandler : HttpMessageHandler
    {
        private readonly Func<HttpRequestMessage, HttpResponseMessage> _respond;

        public StubHandler(Func<HttpRequestMessage, HttpResponseMessage> respond) => _respond = respond;

        public static StubHandler Json(string json) => new(_ => new HttpResponseMessage(HttpStatusCode.OK)
        {
            Content = new StringContent(json)
        });

        public static StubHandler Status(HttpStatusCode status) => new(_ => new HttpResponseMessage(status));

        public static StubHandler Throws(Exception exception) => new(_ => throw exception);

        protected override Task<HttpResponseMessage> SendAsync(
            HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(_respond(request));
    }

    private const string FeedUrl = "https://updates.example.com/latest.json";

    private static string Feed(string version) =>
        $$"""{"version":"{{version}}","msixUrl":"https://x/a.msix","appinstallerUrl":"https://x/a.appinstaller"}""";

    [Fact]
    public async Task EmptyFeedUrl_MeansDisabled()
    {
        using var service = new UpdateCheckService(StubHandler.Throws(new InvalidOperationException("must not be called")));
        Assert.Equal(UpdateCheckStatus.Disabled, (await service.CheckAsync("1.0.0", null)).Status);
        Assert.Equal(UpdateCheckStatus.Disabled, (await service.CheckAsync("1.0.0", "")).Status);
        Assert.Equal(UpdateCheckStatus.Disabled, (await service.CheckAsync("1.0.0", "   ")).Status);
    }

    [Fact]
    public async Task NewerFeedVersion_YieldsUpdateAvailable_WithTheFeedEntry()
    {
        using var service = new UpdateCheckService(StubHandler.Json(Feed("1.1.0")));
        var result = await service.CheckAsync("1.0.0", FeedUrl);

        Assert.Equal(UpdateCheckStatus.UpdateAvailable, result.Status);
        Assert.Equal("1.1.0", result.Update!.Version);
        Assert.Equal("https://x/a.appinstaller", result.Update.DownloadUrl);
    }

    [Theory]
    [InlineData("1.0.0")] // same
    [InlineData("0.9.0")] // older
    public async Task SameOrOlderFeedVersion_YieldsUpToDate(string feedVersion)
    {
        using var service = new UpdateCheckService(StubHandler.Json(Feed(feedVersion)));
        var result = await service.CheckAsync("1.0.0", FeedUrl);
        Assert.Equal(UpdateCheckStatus.UpToDate, result.Status);
        Assert.Null(result.Detail);
    }

    [Fact]
    public async Task MalformedFeed_FailsSilently_WithDetail()
    {
        using var service = new UpdateCheckService(StubHandler.Json("<html>certainly not json</html>"));
        var result = await service.CheckAsync("1.0.0", FeedUrl);
        Assert.Equal(UpdateCheckStatus.Failed, result.Status);
        Assert.Contains("malformed", result.Detail);
    }

    [Fact]
    public async Task UnreachableFeed_FailsSilently_NeverThrows()
    {
        using var service = new UpdateCheckService(StubHandler.Throws(new HttpRequestException("connection refused")));
        var result = await service.CheckAsync("1.0.0", FeedUrl);
        Assert.Equal(UpdateCheckStatus.Failed, result.Status);
        Assert.Contains("connection refused", result.Detail);
    }

    [Fact]
    public async Task HttpErrorStatus_FailsSilently()
    {
        using var service = new UpdateCheckService(StubHandler.Status(HttpStatusCode.InternalServerError));
        var result = await service.CheckAsync("1.0.0", FeedUrl);
        Assert.Equal(UpdateCheckStatus.Failed, result.Status);
    }

    [Fact]
    public async Task UnparseableCurrentVersion_Fails_InsteadOfNagging()
    {
        using var service = new UpdateCheckService(StubHandler.Json(Feed("9.9.9")));
        var result = await service.CheckAsync("dev-build", FeedUrl);
        Assert.Equal(UpdateCheckStatus.Failed, result.Status);
    }

    [Fact]
    public async Task LocalFilePathAndFileUri_AreSupportedForRigTests()
    {
        var dir = Directory.CreateTempSubdirectory("cvp-update-feed-").FullName;
        try
        {
            var path = Path.Combine(dir, "latest.json");
            await File.WriteAllTextAsync(path, Feed("2.0.0"));

            using var service = new UpdateCheckService(StubHandler.Throws(new InvalidOperationException("no http expected")));

            var byPath = await service.CheckAsync("1.0.0", path);
            Assert.Equal(UpdateCheckStatus.UpdateAvailable, byPath.Status);

            var byUri = await service.CheckAsync("1.0.0", new Uri(path).AbsoluteUri);
            Assert.Equal(UpdateCheckStatus.UpdateAvailable, byUri.Status);

            var missing = await service.CheckAsync("1.0.0", Path.Combine(dir, "nope.json"));
            Assert.Equal(UpdateCheckStatus.Failed, missing.Status);
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }
}
