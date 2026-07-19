using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class FileZoomTokenStoreTests
{
    // Reversible fake matching the DpapiSecretProtector contract: recognizable
    // prefix, pass-through of unprefixed (plaintext) values on decrypt.
    private const string Prefix = "fake-enc:";

    private static string FakeEncrypt(string value) =>
        value.StartsWith(Prefix, StringComparison.Ordinal) ? value : Prefix + Reverse(value);

    private static string FakeDecrypt(string value) =>
        value.StartsWith(Prefix, StringComparison.Ordinal) ? Reverse(value[Prefix.Length..]) : value;

    private static string Reverse(string value)
    {
        var chars = value.ToCharArray();
        Array.Reverse(chars);
        return new string(chars);
    }

    private static string TempTokenPath() => Path.Combine(
        Path.GetTempPath(), "corevideo-token-store-tests", Guid.NewGuid().ToString("N"), "zoom-oauth.json");

    [Fact]
    public async Task SaveThenLoad_RoundTripsThroughEncryption()
    {
        var path = TempTokenPath();
        var store = new FileZoomTokenStore(path, FakeEncrypt, FakeDecrypt);

        await store.SaveAsync(new ZoomOAuthTokens
        {
            AccessToken = "access-1",
            RefreshToken = "refresh-1",
            ExpiresAt = 1234567890
        });

        var raw = await File.ReadAllTextAsync(path);
        Assert.DoesNotContain("access-1", raw, StringComparison.Ordinal);
        Assert.DoesNotContain("refresh-1", raw, StringComparison.Ordinal);
        Assert.Contains(Prefix, raw, StringComparison.Ordinal);

        var loaded = await store.LoadAsync();
        Assert.NotNull(loaded);
        Assert.Equal("access-1", loaded.AccessToken);
        Assert.Equal("refresh-1", loaded.RefreshToken);
        Assert.Equal(1234567890, loaded.ExpiresAt);
    }

    [Fact]
    public async Task Load_PlaintextLegacyFile_AcceptsTokensAndResavesEncrypted()
    {
        var path = TempTokenPath();
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        await File.WriteAllTextAsync(path, JsonSerializer.Serialize(new
        {
            AccessToken = "legacy-access",
            RefreshToken = "legacy-refresh",
            ExpiresAt = 42L
        }));

        var store = new FileZoomTokenStore(path, FakeEncrypt, FakeDecrypt);

        // Never lose a working token: plaintext must load...
        var loaded = await store.LoadAsync();
        Assert.NotNull(loaded);
        Assert.Equal("legacy-access", loaded.AccessToken);
        Assert.Equal("legacy-refresh", loaded.RefreshToken);
        Assert.Equal(42, loaded.ExpiresAt);

        // ...and the load must have rewritten the file encrypted.
        var raw = await File.ReadAllTextAsync(path);
        Assert.DoesNotContain("legacy-access", raw, StringComparison.Ordinal);
        Assert.DoesNotContain("legacy-refresh", raw, StringComparison.Ordinal);
        Assert.Contains(Prefix, raw, StringComparison.Ordinal);

        // The migrated file still loads.
        var reloaded = await store.LoadAsync();
        Assert.NotNull(reloaded);
        Assert.Equal("legacy-access", reloaded.AccessToken);
        Assert.Equal("legacy-refresh", reloaded.RefreshToken);
    }

    [Fact]
    public async Task Load_EncryptedFile_DoesNotRewrite()
    {
        var path = TempTokenPath();
        var store = new FileZoomTokenStore(path, FakeEncrypt, FakeDecrypt);
        await store.SaveAsync(new ZoomOAuthTokens
        {
            AccessToken = "access-2",
            RefreshToken = "refresh-2",
            ExpiresAt = 99
        });

        var writtenAt = File.GetLastWriteTimeUtc(path);
        var rawBefore = await File.ReadAllTextAsync(path);

        var loaded = await store.LoadAsync();

        Assert.NotNull(loaded);
        Assert.Equal(rawBefore, await File.ReadAllTextAsync(path));
        Assert.Equal(writtenAt, File.GetLastWriteTimeUtc(path));
    }

    [Fact]
    public async Task Load_WithoutDelegates_KeepsPlaintextBehavior()
    {
        var path = TempTokenPath();
        var store = new FileZoomTokenStore(path);
        await store.SaveAsync(new ZoomOAuthTokens
        {
            AccessToken = "plain-access",
            RefreshToken = "plain-refresh",
            ExpiresAt = 7
        });

        var raw = await File.ReadAllTextAsync(path);
        Assert.Contains("plain-access", raw, StringComparison.Ordinal);

        var loaded = await store.LoadAsync();
        Assert.NotNull(loaded);
        Assert.Equal("plain-access", loaded.AccessToken);
    }

    [Fact]
    public async Task RoundTrip_EmptyRefreshToken_Survives()
    {
        // SaveTokenResponseAsync can legitimately persist an empty refresh token;
        // empty values bypass the encrypt delegate and must round-trip.
        var path = TempTokenPath();
        var store = new FileZoomTokenStore(path, FakeEncrypt, FakeDecrypt);

        await store.SaveAsync(new ZoomOAuthTokens
        {
            AccessToken = "access-3",
            RefreshToken = "",
            ExpiresAt = 5
        });

        var loaded = await store.LoadAsync();
        Assert.NotNull(loaded);
        Assert.Equal("access-3", loaded.AccessToken);
        Assert.Equal("", loaded.RefreshToken);
    }
}
