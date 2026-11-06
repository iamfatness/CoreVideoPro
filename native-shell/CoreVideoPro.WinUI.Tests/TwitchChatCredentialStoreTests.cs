using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class TwitchChatCredentialStoreTests
{
    [Fact]
    public void RotatedPairIsProtectedTogetherAndPlaintextIsRefused()
    {
        var directory = Path.Combine(Path.GetTempPath(), "CoreVideoPro-twitch-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var store = new TwitchChatCredentialStore(directory);
            var first = new TwitchCredentials("fixtureclientid1234", "fixture-access-token", "fixture-refresh-token", DateTimeOffset.UtcNow.AddHours(1), "5678", "fixtureuser");
            Assert.Null(store.Read());
            store.Store(first);
            Assert.Equal(first, store.Read());
            var file = Assert.Single(Directory.EnumerateFiles(directory));
            var stored = File.ReadAllText(file);
            Assert.DoesNotContain(first.AccessToken, stored);
            Assert.DoesNotContain(first.RefreshToken, stored);
            var rotated = first with { AccessToken = "fixture-rotated-access", RefreshToken = "fixture-rotated-refresh" };
            store.Store(rotated);
            Assert.Equal(rotated, store.Read());
            File.WriteAllText(file, JsonSerializer.Serialize(rotated));
            Assert.Null(store.Read());
            store.Clear();
            Assert.Null(store.Read());
        }
        finally
        {
            foreach (var file in Directory.EnumerateFiles(directory)) File.Delete(file);
            Directory.Delete(directory);
        }
    }
}
