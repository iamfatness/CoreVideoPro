using System.Security.Cryptography;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

internal sealed class YouTubeChatCredentialStore
{
    private readonly AtomicJsonFile _file;

    public YouTubeChatCredentialStore() : this(Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro", "graphics", "social")) { }

    internal YouTubeChatCredentialStore(string directory) => _file = new(Path.Combine(directory, "youtube-key.json"));

    public void Store(string key)
    {
        if (!YouTubeSocialFeed.IsApiKey(key)) throw new InvalidDataException("Enter a YouTube Data API key.");
        var protectedKey = DpapiSecretProtector.Protect(key);
        _file.Locked(() => { _file.Write(JsonSerializer.Serialize(protectedKey), null); return true; });
    }

    public string? Read() => _file.Locked(() =>
    {
        try
        {
            using var stream = File.OpenRead(_file.Path);
            if (stream.Length > 4096) return null;
            var stored = JsonSerializer.Deserialize<string>(stream);
            if (!DpapiSecretProtector.IsProtected(stored)) return null;
            var key = DpapiSecretProtector.Unprotect(stored!);
            return YouTubeSocialFeed.IsApiKey(key) ? key : null;
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or CryptographicException or FormatException)
        { return null; }
    });

    public void Clear() => _file.Locked(() =>
    {
        File.Delete(_file.Path);
        return true;
    });
}
