using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class GraphicsFeedCredentialStoreTests : IDisposable
{
    private static readonly GraphicsFeedDefinition Feed = new("scores", "Scores", "custom-json", "https://example.com/scores", "id");
    private const string FixtureBearer = "fixture-first-token";
    private const string RotatedBearer = "fixture-second-token";
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-feed-credentials-" + Guid.NewGuid().ToString("N"));

    [Fact]
    public void DpapiRoundTripBindsTheReferenceToShowFeedFormatAndExactEndpoint()
    {
        var store = new GraphicsFeedCredentialStore(_directory);
        var reference = store.Store("show", Feed, FixtureBearer);
        var bound = Feed with { CredentialReference = reference };
        Assert.Equal(FixtureBearer, new GraphicsFeedCredentialStore(_directory).Resolve("show", bound));
        var blob = File.ReadAllText(Assert.Single(Directory.GetFiles(_directory)));
        Assert.Contains(DpapiSecretProtector.Prefix, blob);
        Assert.DoesNotContain(FixtureBearer, blob);
        Assert.DoesNotContain(Feed.Endpoint, blob);
        Assert.Null(store.Resolve("imported-show", bound));
        Assert.Null(store.Resolve("show", bound with { Id = "other" }));
        Assert.Null(store.Resolve("show", bound with { Endpoint = Feed.Endpoint + "/" }));
        Assert.Null(store.Resolve("show", bound with { Format = "wss" }));
        Assert.Null(new GraphicsFeedCredentialStore(Path.Combine(_directory, "imported")).Resolve("show", bound));
    }

    [Fact]
    public async Task RotationSavesOnlyAReferenceAndEndpointEditsClearIt()
    {
        var store = new GraphicsFeedCredentialStore(Path.Combine(_directory, "secrets"));
        var path = Path.Combine(_directory, "graphics.json");
        var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(path),
            new GraphicsWorkspaceSession((_, _, _) => throw new InvalidOperationException(), () => null));
        var first = Assert.IsType<GraphicsFeedDefinition>(await store.SaveAsync(library, Feed, FixtureBearer));
        var rotated = Assert.IsType<GraphicsFeedDefinition>(await store.SaveAsync(library, first, RotatedBearer));
        Assert.NotEqual(first.CredentialReference, rotated.CredentialReference);
        Assert.Null(store.Resolve(library.Document.ShowId, first));
        Assert.Equal(RotatedBearer, store.Resolve(library.Document.ShowId, rotated));
        var savedJson = await File.ReadAllTextAsync(path);
        Assert.Contains(rotated.CredentialReference!, savedJson);
        Assert.DoesNotContain(RotatedBearer, savedJson);
        Assert.DoesNotContain(DpapiSecretProtector.Prefix, savedJson);

        Assert.Null(await store.SaveAsync(library, first, FixtureBearer));
        Assert.Single(Directory.GetFiles(Path.Combine(_directory, "secrets")));
        var edited = Assert.IsType<GraphicsFeedDefinition>(await store.SaveAsync(library,
            rotated with { Endpoint = "https://example.com/another-scoreboard" }));
        Assert.Null(edited.CredentialReference);
        Assert.Null(store.Resolve(library.Document.ShowId, rotated));
    }

    [Fact]
    public void PlaintextAndInvalidBearerValuesAreRefused()
    {
        var store = new GraphicsFeedCredentialStore(_directory);
        Assert.Throws<InvalidDataException>(() => store.Store("show", Feed, "fixture\r\nX-Fake: header"));
        var reference = store.Store("show", Feed, FixtureBearer);
        File.WriteAllText(Assert.Single(Directory.GetFiles(_directory)), "\"plaintext\"");
        Assert.Null(store.Resolve("show", Feed with { CredentialReference = reference }));
    }

    public void Dispose()
    {
        if (!Directory.Exists(_directory)) return;
        foreach (var file in Directory.EnumerateFiles(_directory, "*", SearchOption.AllDirectories)) File.Delete(file);
        foreach (var folder in Directory.EnumerateDirectories(_directory)) Directory.Delete(folder);
        Directory.Delete(_directory);
    }
}
