using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
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

    [Theory]
    [InlineData(ZoomSdkReadinessStatus.Ready)]
    [InlineData(ZoomSdkReadinessStatus.Warning)]
    public void NonBlockingSdkWarnings_DoNotBecomeOperatorWarnings(ZoomSdkReadinessStatus status)
    {
        Assert.Equal("Ready to join Zoom", SettingsViewModel.FormatZoomOperatorStatus(status, isInMeeting: false));
        Assert.Equal(
            "Enter a meeting link or ID above when you are ready to connect.",
            SettingsViewModel.FormatZoomOperatorGuidance(status, isInMeeting: false));
    }

    [Theory]
    [InlineData(ZoomSdkReadinessStatus.Ready)]
    [InlineData(ZoomSdkReadinessStatus.Warning)]
    [InlineData(ZoomSdkReadinessStatus.Blocked)]
    public void ActiveMeeting_IsAlwaysReportedAsConnected(ZoomSdkReadinessStatus status)
    {
        Assert.Equal("Connected to Zoom", SettingsViewModel.FormatZoomOperatorStatus(status, isInMeeting: true));
        Assert.Equal("Zoom is connected and working.", SettingsViewModel.FormatZoomOperatorGuidance(status, isInMeeting: true));
    }
}
