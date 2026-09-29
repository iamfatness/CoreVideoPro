using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ZoomCameraResolutionPreferenceTests
{
    [Theory]
    [InlineData("360p", 0)]
    [InlineData("720p", 1)]
    [InlineData("1080p", 2)]
    [InlineData("unknown", 2)]
    [InlineData(null, 2)]
    public void OnlySupportedCameraCeilingsReachTheSdk(string? choice, int tier)
    {
        Assert.Equal(tier, ZoomCameraResolutionPreference.ToSdkTier(choice));
        Assert.Equal(ZoomCameraResolutionPreference.FromSdkTier(tier),
            ZoomCameraResolutionPreference.Normalize(choice));
    }

    [Fact]
    public void ExistingProfileDefaultsTo1080AndAnExplicitChoicePersists()
    {
        var old = ProductionOutputPreferencesSerializer.Deserialize("{\"Version\":13}", out var migrated);
        Assert.NotNull(old);
        Assert.True(migrated);
        Assert.Equal("1080p", old.ZoomCameraMaxResolution);

        old.ZoomCameraMaxResolution = "360p";
        var saved = ProductionOutputPreferencesSerializer.Serialize(old);
        var restored = ProductionOutputPreferencesSerializer.Deserialize(saved);
        Assert.NotNull(restored);
        Assert.Equal("360p", restored.ZoomCameraMaxResolution);
    }

    [Fact]
    public void MidMeetingChangeIsShownAsPendingUntilNextJoin()
    {
        Assert.Contains("720p pending for next Zoom join",
            ZoomCameraResolutionPreference.Status("720p", "1080p", true));
        Assert.Contains("Requested max 720p this meeting",
            ZoomCameraResolutionPreference.Status("720p", "720p", true));
        Assert.Contains("Format shows actual delivery",
            ZoomCameraResolutionPreference.Status("360p", "1080p", false));
    }

    [Theory]
    [InlineData("360p", 640, 360)]
    [InlineData("720p", 1280, 720)]
    [InlineData("1080p", 1920, 1080)]
    public void CameraCapDimensionsMatchRequestedTier(string choice, int width, int height)
    {
        Assert.Equal((width, height), ZoomCameraResolutionPreference.Dimensions(choice));
    }

    [Fact]
    public void GuestFrameRateCeilingPersistsAndLabelsLocalBehavior()
    {
        var old = ProductionOutputPreferencesSerializer.Deserialize("{\"Version\":13}");
        Assert.NotNull(old);
        Assert.Equal(60, ZoomCameraFrameRatePreference.Normalize(old.ZoomCameraMaxFps));
        old.ZoomCameraMaxFps = 30;
        var restored = ProductionOutputPreferencesSerializer.Deserialize(
            ProductionOutputPreferencesSerializer.Serialize(old));
        Assert.NotNull(restored);
        Assert.Equal(30, restored.ZoomCameraMaxFps);
        Assert.Contains("Zoom may deliver more",
            ZoomCameraFrameRatePreference.Status(30, 30, true));
        Assert.Contains("pending for next Zoom join",
            ZoomCameraFrameRatePreference.Status(24, 30, true));
    }
}
