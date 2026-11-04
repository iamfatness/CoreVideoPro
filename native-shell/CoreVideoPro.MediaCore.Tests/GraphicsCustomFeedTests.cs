using System.Net;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsCustomFeedTests
{
    private static readonly GraphicsFeedDefinition Feed = new("scores", "Scores", "custom-json", "https://example.com/scores", "id");
    private const string FixtureReference = "177e7ce868d94c47af4eeb2736dd7bea";
    private const string FixtureBearer = "fixture-token";

    [Fact]
    public async Task ReadsAnAnonymousRootArray()
    {
        using var transport = new GraphicsFeedTransport((request, _) =>
        {
            Assert.Null(request.Headers.Authorization);
            return Response("""[{"id":"a","score":12}]""");
        }, resolveCredential: _ => throw new InvalidOperationException("Unexpected credential lookup."));
        var data = await transport.FetchAsync(Feed, default);
        Assert.Equal("a", Assert.Single(data.Rows).Key);
        Assert.Equal(new GraphicsDataColumn("score", "number"), data.Columns[1]);
    }

    [Fact]
    public async Task ProjectsNestedRowsWithEscapedPropertiesAndArrayIndices()
    {
        using var transport = FromBody("""{"data":{"result/sets":[null,[{"team~id":"a","team":[{"name":"Ada"}],"count":12,"active":true}]]}}""");
        var feed = Feed with { Mapping = new("/data/result~1sets/1",
            [new("id", "/team~0id"), new("team", "/team/0/name"), new("points", "/count"), new("active", "/active")]) };
        var data = await transport.FetchAsync(feed, default);
        Assert.Equal(new[] { "text", "text", "number", "boolean" }, data.Columns.Select(column => column.Kind));
        Assert.Equal(new[] { "a", "Ada", "12", "true" }, Assert.Single(data.Rows).Cells);
    }

    [Theory]
    [InlineData("/team/1/name")]
    [InlineData("/team/00/name")]
    [InlineData("/team/2147483648/name")]
    [InlineData("/team/-/name")]
    [InlineData("/team")]
    [InlineData("/missing")]
    [InlineData("/~2")]
    [InlineData("#/team/0/name")]
    public async Task RejectsInvalidPathsAndNonscalarCells(string pointer)
    {
        using var transport = FromBody("""[{"key":"a","team":[{"name":"Ada"}]}]""");
        await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(
            Feed with { Mapping = new("", [new("id", "/key"), new("name", pointer)]) }, default));
    }

    [Theory]
    [InlineData("""[{"key":"a"},{"key":"a"}]""")]
    [InlineData("""[{"key":"a","key":"b"}]""")]
    [InlineData("""[{"key":""}]""")]
    public async Task RejectsDuplicateOrEmptyMappedKeys(string body)
    {
        using var transport = FromBody(body);
        await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(
            Feed with { Mapping = new("", [new("id", "/key")]) }, default));
    }

    [Fact]
    public async Task CredentialedGetRefusesRedirectAndSanitizesProviderErrors()
    {
        var requests = 0;
        var feed = Feed with { CredentialReference = FixtureReference };
        using var transport = new GraphicsFeedTransport((request, _) =>
        {
            requests++;
            Assert.Equal(HttpMethod.Get, request.Method);
            Assert.Equal(feed.Endpoint, request.RequestUri!.AbsoluteUri);
            Assert.Equal("Bearer " + FixtureBearer, request.Headers.Authorization?.ToString());
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.Redirect)
            {
                Headers = { Location = new Uri("https://other.example.com/private") },
                ReasonPhrase = FixtureBearer
            });
        }, resolveCredential: definition => { Assert.Equal(feed, definition); return FixtureBearer; });
        var error = await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(feed, default));
        Assert.Equal(1, requests);
        Assert.DoesNotContain(FixtureBearer, error.ToString());
        Assert.DoesNotContain("other.example.com", error.ToString());
    }

    [Fact]
    public async Task DeniedEndpointDoesNotResolveAndMissingReferenceDoesNotSend()
    {
        var lookups = 0;
        using var transport = new GraphicsFeedTransport((_, _) => throw new InvalidOperationException("Unexpected request."),
            resolveCredential: _ => { lookups++; return null; });
        var feed = Feed with { CredentialReference = FixtureReference };
        await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(
            feed with { Endpoint = "https://127.0.0.1/scores" }, default));
        Assert.Equal(0, lookups);
        await Assert.ThrowsAsync<GraphicsFeedCredentialException>(() => transport.FetchAsync(feed, default));
        Assert.Equal(1, lookups);
    }

    [Fact]
    public async Task LateResponseFromReplacedMappingCannotReachStaging()
    {
        var root = Path.Combine(Path.GetTempPath(), "cvp-custom-feed-" + Guid.NewGuid().ToString("N"));
        var document = Path.Combine(root, "graphics.json");
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(document),
            new GraphicsWorkspaceSession((_, _, _) => throw new InvalidOperationException(), () => null));
        try
        {
            var before = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(Feed with
            {
                Enabled = true, CredentialReference = FixtureReference, Mapping = new("/old", [])
            }));
            var started = new TaskCompletionSource<CancellationToken>(TaskCreationOptions.RunContinuationsAsynchronously);
            var held = new TaskCompletionSource<HttpResponseMessage>(TaskCreationOptions.RunContinuationsAsynchronously);
            using var transport = new GraphicsFeedTransport((request, cancellation) =>
            {
                if (request.RequestUri!.AbsoluteUri == Feed.Endpoint)
                {
                    started.SetResult(cancellation);
                    return held.Task;
                }
                Assert.Null(request.Headers.Authorization);
                return Response("""{"new":[{"id":"a","score":23}]}""");
            }, resolveCredential: _ => FixtureBearer);
            var clock = new Clock();
            var staged = new List<GraphicsFeedDefinition>();
            using var poller = new GraphicsFeedPoller(transport.FetchAsync, (feed, data, token) =>
            {
                staged.Add(feed);
                return library.StageFeedAsync(feed, data, clock.GetUtcNow(), token);
            }, clock);
            poller.Configure(before);
            var oldRequest = poller.TickAsync();
            var oldToken = await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var after = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(before with
            {
                Endpoint = "https://example.com/new-scores", Mapping = new("/new", [])
            }));
            Assert.Null(after.CredentialReference);
            poller.Configure(after);
            Assert.True(oldToken.IsCancellationRequested);
            held.SetResult(await Response("""{"old":[{"id":"a","score":99}]}"""));
            await oldRequest;
            Assert.Empty(staged);
            clock.Now = clock.Now.AddSeconds(5);
            await poller.TickAsync();
            Assert.Equal(after, Assert.Single(staged));
            var reopened = (await new GraphicsDocumentStore(document).LoadAsync()).Document;
            Assert.Equal("/new", Assert.Single(reopened.Feeds).Mapping!.RowsPointer);
            Assert.Equal("23", Assert.Single(Assert.Single(reopened.DataSources).Data.Rows).Cells[1]);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                foreach (var file in Directory.GetFiles(root)) File.Delete(file);
                Directory.Delete(root);
            }
        }
    }

    [Fact]
    public async Task CredentialAndProviderFailuresExposeOnlySanitizedStatus()
    {
        var clock = new Clock();
        using var transport = new GraphicsFeedTransport((_, _) => Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.Forbidden) { ReasonPhrase = FixtureBearer }),
            resolveCredential: _ => throw new IOException(FixtureBearer));
        using var poller = new GraphicsFeedPoller(transport.FetchAsync,
            (_, _, _) => throw new InvalidOperationException("Unexpected stage."), clock);
        poller.Configure(Feed with { CredentialReference = FixtureReference, Enabled = true });
        await poller.TickAsync();
        Assert.Equal("credentials-required", poller.Status.Failure);
        Assert.DoesNotContain(FixtureBearer, JsonSerializer.Serialize(poller.Status));
        var rejected = await Assert.ThrowsAsync<HttpRequestException>(() => transport.FetchAsync(Feed, default));
        Assert.DoesNotContain(FixtureBearer, rejected.ToString());
        Assert.Null(rejected.InnerException);
    }

    [Fact]
    public async Task MappedProjectionCannotExpandBeyondTheByteLimit()
    {
        var row = new { key = "a", value = new string('x', 4000) };
        var body = JsonSerializer.Serialize(Enumerable.Range(0, 12).Select(index => row with { key = index.ToString() }));
        var columns = new[] { new GraphicsFeedColumnMapping("id", "/key") }
            .Concat(Enumerable.Range(1, 31).Select(index => new GraphicsFeedColumnMapping($"col{index}", "/value"))).ToArray();
        using var transport = FromBody(body);
        var error = await Assert.ThrowsAsync<InvalidDataException>(() => transport.FetchAsync(Feed with { Mapping = new("", columns) }, default));
        Assert.Contains("1 MiB", error.Message);
    }

    private sealed class Clock : TimeProvider
    {
        public DateTimeOffset Now { get; set; } = DateTimeOffset.UnixEpoch;
        public override DateTimeOffset GetUtcNow() => Now;
    }

    private static GraphicsFeedTransport FromBody(string body) => new((_, _) => Response(body));
    private static Task<HttpResponseMessage> Response(string body) =>
        Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new StringContent(body) });
}
