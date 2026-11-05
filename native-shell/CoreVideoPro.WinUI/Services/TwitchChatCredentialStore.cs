using System.Security.Cryptography;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

internal sealed class TwitchChatCredentialStore
{
    private readonly AtomicJsonFile _file;

    public TwitchChatCredentialStore() : this(Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro", "graphics", "social")) { }

    internal TwitchChatCredentialStore(string directory) => _file = new(Path.Combine(directory, "twitch-credentials.json"));

    public void Store(TwitchCredentials credentials)
    {
        if (!Valid(credentials)) throw new InvalidDataException("The Twitch credentials are incomplete.");
        var protectedPair = DpapiSecretProtector.Protect(JsonSerializer.Serialize(credentials));
        _file.Locked(() => { _file.Write(JsonSerializer.Serialize(protectedPair), null); return true; });
    }

    public TwitchCredentials? Read() => _file.Locked(() =>
    {
        try
        {
            using var stream = File.OpenRead(_file.Path);
            if (stream.Length > 65536) return null;
            var stored = JsonSerializer.Deserialize<string>(stream);
            if (!DpapiSecretProtector.IsProtected(stored)) return null;
            var credentials = JsonSerializer.Deserialize<TwitchCredentials>(DpapiSecretProtector.Unprotect(stored!));
            return credentials is not null && Valid(credentials) ? credentials : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or CryptographicException or FormatException)
        { return null; }
    });

    public void Clear() => _file.Locked(() => { File.Delete(_file.Path); return true; });

    private static bool Valid(TwitchCredentials credentials) =>
        credentials.ClientId is not null && TwitchAuthorization.IsClientId(credentials.ClientId) &&
        credentials.UserId is not null && credentials.Login is not null &&
        Token(credentials.AccessToken) && Token(credentials.RefreshToken);
    private static bool Token(string? value) => value is { Length: >= 10 and <= 4096 } && value.All(c => c is >= '!' and <= '~');
}
