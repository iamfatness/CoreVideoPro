using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ZoomOAuthProtocolTests
{
    [Fact]
    public void PrimaryProtocol_MatchesTheBrokerPinnedReturnUri()
    {
        // One scheme end-to-end: the broker allowlists corevideo://oauth/callback,
        // ZoomOAuthManifest defaults to it, and the appxmanifest + HKCU
        // registration declare the matching protocol.
        Assert.Equal("corevideo", ZoomOAuthAppCoordinator.Protocol);
        Assert.StartsWith(
            $"{ZoomOAuthAppCoordinator.Protocol}://",
            ZoomOAuthManifest.DefaultAppReturnUri,
            StringComparison.Ordinal);
    }

    [Fact]
    public void CallbackDetection_AcceptsPrimaryAndLegacySchemes()
    {
        Assert.True(ZoomOAuthAppCoordinator.IsOAuthCallbackUrl("corevideo://oauth/callback?state=s&broker_token=t"));
        Assert.True(ZoomOAuthAppCoordinator.IsOAuthCallbackUrl("corevideopro://oauth/callback?state=s"));
        Assert.False(ZoomOAuthAppCoordinator.IsOAuthCallbackUrl("https://example.com/oauth/callback"));
        Assert.False(ZoomOAuthAppCoordinator.IsOAuthCallbackUrl(null));
    }

    [Fact]
    public void PackageManifest_DeclaresThePrimaryProtocol()
    {
        // Packaged installs receive the broker redirect only via the manifest
        // protocol declaration — pin it so the schemes cannot drift apart again.
        var manifestPath = FindRepoFile(Path.Combine("native-shell", "CoreVideoPro.WinUI", "Package.appxmanifest"));
        var manifest = File.ReadAllText(manifestPath);

        Assert.Contains($"<uap:Protocol Name=\"{ZoomOAuthAppCoordinator.Protocol}\">", manifest, StringComparison.Ordinal);
    }

    private static string FindRepoFile(string relativePath)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, relativePath);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        throw new FileNotFoundException($"Could not locate {relativePath} above {AppContext.BaseDirectory}.");
    }
}
