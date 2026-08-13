using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomOAuthManifestTests
{
    [Fact]
    public void DefaultAppReturnUri_IsPinnedToTheBrokerAllowlist()
    {
        // The deployed broker (corevideo.iamfatness.us site-worker.js,
        // handleOauthStart) rejects any return_uri other than EXACTLY
        // corevideo://oauth/callback with 400 "Unsupported OAuth return URI."
        // Changing this constant without updating the broker allowlist first
        // breaks sign-in for every install. Package.appxmanifest declares the
        // matching "corevideo" protocol.
        Assert.Equal("corevideo://oauth/callback", ZoomOAuthManifest.DefaultAppReturnUri);
    }

    [Fact]
    public void Defaults_UseTheAppReturnUriAsRedirect()
    {
        var manifest = new ZoomOAuthManifest();

        Assert.Equal(ZoomOAuthManifest.DefaultAppReturnUri, manifest.RedirectUri);
    }
}
