using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomOAuthManifestTests
{
    [Fact]
    public void DefaultAppReturnUri_IsPinnedToTheBrokerAllowlist()
    {
        // The deployed broker (site-worker.js, handleOauthStart) rejects any return_uri other than
        // EXACTLY corevideo://oauth/callback with 400 "Unsupported OAuth return URI." Changing this
        // constant without updating the broker allowlist first breaks sign-in for every install. Package.appxmanifest declares the
        // matching "corevideo" protocol.
        Assert.Equal("corevideo://oauth/callback", ZoomOAuthManifest.DefaultAppReturnUri);
    }

    [Fact]
    public void Defaults_UseTheAppReturnUriAsRedirect()
    {
        var manifest = new ZoomOAuthManifest();

        Assert.Equal(ZoomOAuthManifest.DefaultAppReturnUri, manifest.RedirectUri);
    }

    [Fact]
    public void BrokerDefaults_PointAtTheProductionDomain()
    {
        Assert.Equal("https://corevideo.iamfatness.us/oauth/start", ZoomOAuthManifest.DefaultBrokerStartUrl);
        // Must equal the Zoom Marketplace redirect allow-list entry.
        Assert.Equal("https://corevideo.iamfatness.us/oauth/callback", ZoomOAuthManifest.DefaultBrokerCallbackUrl);
    }

    [Fact]
    public void Load_WithoutManifestFileOrOverride_UsesBakedBrokerDefaults()
    {
        var oldValue = Environment.GetEnvironmentVariable(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar);
        var emptyRoot = Directory.CreateTempSubdirectory("cvp-oauth-").FullName;
        try
        {
            Environment.SetEnvironmentVariable(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar, null);
            var manifest = ZoomOAuthManifest.Load(emptyRoot);

            Assert.Equal(ZoomOAuthManifest.DefaultBrokerStartUrl, manifest.BrokerStartUrl);
            Assert.Equal(ZoomOAuthManifest.DefaultBrokerCallbackUrl, manifest.BrokerCallbackUrl);

            Environment.SetEnvironmentVariable(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar, "https://staging.example.test/oauth/start");
            Assert.Equal("https://staging.example.test/oauth/start", ZoomOAuthManifest.Load(emptyRoot).BrokerStartUrl);
        }
        finally
        {
            Environment.SetEnvironmentVariable(MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar, oldValue);
            Directory.Delete(emptyRoot, recursive: true);
        }
    }
}
