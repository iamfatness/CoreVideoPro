using System.Security.Cryptography;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

internal sealed class GraphicsFeedCredentialStore(string directory)
{
    private const int MaxFileBytes = 64 * 1024;

    public GraphicsFeedCredentialStore() : this(Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro", "graphics", "feed-credentials")) { }

    public string Store(string showId, GraphicsFeedDefinition feed, string token)
    {
        GraphicsFeedCredentials.ValidateEndpoint(feed);
        if (!GraphicsFeedCredentials.IsValidBearer(token))
            throw new InvalidDataException("Enter a valid bearer token.");

        var reference = Guid.NewGuid().ToString("N");
        var file = FileFor(reference)!;
        var protectedValue = DpapiSecretProtector.Protect(JsonSerializer.Serialize(new Entry(showId, feed.Id, feed.Format, feed.Endpoint, token)));
        file.Locked(() =>
        {
            file.Write(JsonSerializer.Serialize(protectedValue), null);
            return true;
        });
        return reference;
    }

    public string? Resolve(string showId, GraphicsFeedDefinition feed)
    {
        if (FileFor(feed.CredentialReference) is not { } file) return null;
        return file.Locked(() => ReadMatching(file, showId, feed)?.Token);
    }

    public async Task<GraphicsFeedDefinition?> SaveAsync(GraphicsWorkspaceLibrary library, GraphicsFeedDefinition definition,
        string? replacementBearer = null, bool clearCredential = false)
    {
        await library.LoadAsync();
        var showId = library.Document.ShowId;
        var previous = library.Document.Feeds.FirstOrDefault(feed => feed.Id == definition.Id);
        string? newReference = null;
        if (replacementBearer is not null)
        {
            newReference = await Task.Run(() => Store(showId, definition with { CredentialReference = null }, replacementBearer));
            definition = definition with { CredentialReference = newReference };
        }
        else if (clearCredential)
        {
            definition = definition with { CredentialReference = null };
        }

        GraphicsFeedDefinition? saved = null;
        try
        {
            saved = await library.SaveFeedAsync(definition);
            return saved;
        }
        finally
        {
            if (saved is null && newReference is not null)
                await Task.Run(() => RemoveMatching(showId, definition));
            else if (saved is not null && previous?.CredentialReference is { } old && old != saved.CredentialReference)
                await Task.Run(() => RemoveMatching(showId, previous));
        }
    }

    private AtomicJsonFile? FileFor(string? reference) => Guid.TryParseExact(reference, "N", out var id)
        ? new AtomicJsonFile(Path.Combine(directory, id.ToString("N") + ".json")) : null;

    private void RemoveMatching(string showId, GraphicsFeedDefinition feed)
    {
        if (FileFor(feed.CredentialReference) is not { } file) return;
        try
        {
            file.Locked(() =>
            {
                if (ReadMatching(file, showId, feed) is not null) File.Delete(file.Path);
                return true;
            });
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }

    private static Entry? ReadMatching(AtomicJsonFile file, string showId, GraphicsFeedDefinition feed)
    {
        try
        {
            using var stream = File.OpenRead(file.Path);
            if (stream.Length > MaxFileBytes) return null;
            var stored = JsonSerializer.Deserialize<string>(stream);
            if (!DpapiSecretProtector.IsProtected(stored)) return null;
            var entry = JsonSerializer.Deserialize<Entry>(DpapiSecretProtector.Unprotect(stored!));
            return entry is not null && entry.ShowId == showId && entry.FeedId == feed.Id && entry.Format == feed.Format &&
                entry.Endpoint == feed.Endpoint && GraphicsFeedCredentials.IsValidBearer(entry.Token) ? entry : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or CryptographicException or FormatException)
        {
            return null;
        }
    }

    private sealed record Entry(string ShowId, string FeedId, string Format, string Endpoint, string Token);
}
