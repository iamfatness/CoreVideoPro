using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class YouTubeChatCredentialStoreTests
{
    [Fact]
    public void KeyIsProtectedAndClearRemovesTheLocalCopy()
    {
        var directory = Path.Combine(Path.GetTempPath(), "CoreVideoPro-social-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        const string key = "fixture-api-key-abcdefghijklmnop";
        try
        {
            var store = new YouTubeChatCredentialStore(directory);
            Assert.Null(store.Read());
            store.Store(key);
            Assert.Equal(key, store.Read());
            var stored = Assert.Single(Directory.EnumerateFiles(directory));
            Assert.DoesNotContain(key, File.ReadAllText(stored), StringComparison.Ordinal);
            store.Clear();
            Assert.Null(store.Read());
            Assert.Empty(Directory.EnumerateFiles(directory));
        }
        finally
        {
            // The path is a unique test fixture and is empty after successful cleanup.
            foreach (var file in Directory.EnumerateFiles(directory)) File.Delete(file);
            Directory.Delete(directory);
        }
    }
}
