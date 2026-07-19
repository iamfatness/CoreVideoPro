using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class ZoomOAuthTokens
{
    public required string AccessToken { get; init; }
    public required string RefreshToken { get; init; }
    public long ExpiresAt { get; init; }
}

public interface IZoomTokenStore
{
    Task<ZoomOAuthTokens?> LoadAsync(CancellationToken cancellationToken = default);
    Task SaveAsync(ZoomOAuthTokens tokens, CancellationToken cancellationToken = default);
    Task ClearAsync(CancellationToken cancellationToken = default);
}

public sealed class FileZoomTokenStore : IZoomTokenStore
{
    private readonly string _filePath;
    private readonly Func<string, string>? _encrypt;
    private readonly Func<string, string>? _decrypt;

    public FileZoomTokenStore(
        string filePath,
        Func<string, string>? encrypt = null,
        Func<string, string>? decrypt = null)
    {
        _filePath = filePath;
        _encrypt = encrypt;
        _decrypt = decrypt;
    }

    public static string DefaultTokenStorePath()
    {
        var root = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro");
        return Path.Combine(root, "zoom-oauth.json");
    }

    public async Task<ZoomOAuthTokens?> LoadAsync(CancellationToken cancellationToken = default)
    {
        if (!File.Exists(_filePath))
        {
            return null;
        }

        try
        {
            StoredTokens? parsed;
            // The stream must be disposed before the migration re-save below —
            // File.Create on a file still open for read is a sharing violation.
            await using (var stream = File.OpenRead(_filePath))
            {
                parsed = await JsonSerializer.DeserializeAsync<StoredTokens>(stream, cancellationToken: cancellationToken)
                    .ConfigureAwait(false);
            }

            if (parsed?.AccessToken is null || parsed.RefreshToken is null)
            {
                return null;
            }

            var tokens = new ZoomOAuthTokens
            {
                AccessToken = Decrypt(parsed.AccessToken),
                RefreshToken = Decrypt(parsed.RefreshToken),
                ExpiresAt = parsed.ExpiresAt ?? 0
            };

            // Migration (beta spec S4): a legacy plaintext file passes through the
            // decrypt delegate unchanged. When an encrypt delegate is configured,
            // re-save so the secrets land encrypted at rest. A failed rewrite must
            // never lose a working token, so it is best-effort.
            if (_encrypt is not null &&
                (WasStoredPlaintext(parsed.AccessToken, tokens.AccessToken) ||
                 WasStoredPlaintext(parsed.RefreshToken, tokens.RefreshToken)))
            {
                try
                {
                    await SaveAsync(tokens, cancellationToken).ConfigureAwait(false);
                }
                catch
                {
                    // Keep serving the loaded tokens even if the encrypted rewrite failed.
                }
            }

            return tokens;
        }
        catch
        {
            return null;
        }
    }

    public async Task SaveAsync(ZoomOAuthTokens tokens, CancellationToken cancellationToken = default)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_filePath)!);
        var stored = new StoredTokens
        {
            AccessToken = Encrypt(tokens.AccessToken),
            RefreshToken = Encrypt(tokens.RefreshToken),
            ExpiresAt = tokens.ExpiresAt
        };
        await using var stream = File.Create(_filePath);
        await JsonSerializer.SerializeAsync(stream, stored, cancellationToken: cancellationToken).ConfigureAwait(false);
    }

    public Task ClearAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            if (File.Exists(_filePath))
            {
                File.WriteAllText(_filePath, "{}");
            }
        }
        catch
        {
            // ignore
        }

        return Task.CompletedTask;
    }

    private string Encrypt(string value) =>
        string.IsNullOrEmpty(value) ? value : _encrypt?.Invoke(value) ?? value;

    private string Decrypt(string value) =>
        string.IsNullOrEmpty(value) ? value : _decrypt?.Invoke(value) ?? value;

    // A non-empty stored value that the decrypt delegate passed through unchanged
    // was plaintext at rest (empty values are stored as-is and prove nothing).
    private static bool WasStoredPlaintext(string stored, string decrypted) =>
        stored.Length > 0 && string.Equals(stored, decrypted, StringComparison.Ordinal);

    private sealed class StoredTokens
    {
        public string? AccessToken { get; init; }
        public string? RefreshToken { get; init; }
        public long? ExpiresAt { get; init; }
    }
}