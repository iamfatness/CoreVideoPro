using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StreamingProfileCatalogTests
{
    [Fact]
    public void CustomProfile_IsGenericAndDoesNotOverrideEncoderValues()
    {
        var custom = StreamingProfileCatalog.Find(StreamingProfileCatalog.CustomProfileId);

        Assert.Equal("Custom RTMP / RTMPS", custom.Label);
        Assert.Equal(string.Empty, custom.Resolution);
        Assert.Equal(0, custom.TargetBitrateMbps);
    }

    [Theory]
    [InlineData("youtube-1080p60", "1920x1080", "60", 12, 128, "high", 2)]
    [InlineData("twitch-1080p60", "1920x1080", "60", 6, 160, "high", 2)]
    [InlineData("facebook-1080p30", "1920x1080", "30", 6, 128, "high", 2)]
    [InlineData("linkedin-720p30", "1280x720", "30", 3.5, 128, "baseline", 0)]
    public void PublishedProfiles_CarryExpectedSafeDefaults(
        string id,
        string resolution,
        string fps,
        double videoMbps,
        int audioKbps,
        string h264Profile,
        int bFrames)
    {
        var profile = StreamingProfileCatalog.Find(id);

        Assert.Equal(resolution, profile.Resolution);
        Assert.Equal(fps, profile.Fps);
        Assert.Equal(videoMbps, profile.TargetBitrateMbps);
        Assert.Equal(audioKbps, profile.AudioBitrateKbps);
        Assert.Equal(2, profile.KeyframeIntervalSeconds);
        Assert.Equal("cbr", profile.RateControl);
        Assert.Equal(h264Profile, profile.H264Profile);
        Assert.Equal(bFrames, profile.BFrames);
        Assert.Equal("rtmps", profile.Protocol);
    }
}
