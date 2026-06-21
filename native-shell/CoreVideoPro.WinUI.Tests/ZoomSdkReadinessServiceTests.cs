using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ZoomSdkReadinessServiceTests
{
    [Fact]
    public void BlockedReadinessBlocksJoinBeforeEngineStarts()
    {
        var report = ZoomSdkReadinessService.Assess(new ZoomSdkReadinessInput
        {
            Platform = ZoomSdkRuntimePlatform.Windows,
            SdkRuntimePresent = false,
            AppKeyPresent = true,
            OauthConfigured = true,
            OAuthSignedIn = true,
            OAuthBrokerConfigured = true,
            RawVideoEnabled = true,
            RawAudioEnabled = true,
            RawShareEnabled = true,
            NativeCorePresent = true,
            StagedRuntimeReady = false,
            StagingTargetPath = "C:\\CoreVideo\\zoom-runtime\\windows\\x64"
        });

        Assert.True(ZoomSdkReadinessService.ShouldBlockZoomJoin(false, report));
    }
}
