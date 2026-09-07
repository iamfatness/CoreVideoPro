using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ProgramBufferPreferencesTests
{
    [Fact]
    public void ChangedSelectionShowsPendingDepthWithoutClaimingNativeActivation()
    {
        Assert.True(ProgramBufferSettingsSummary.RequiresRestart(3, 2));
        Assert.Equal("This app session: 3 frames requested. Restart the app to apply 2 frames.",
            ProgramBufferSettingsSummary.Describe(3, 2));
        Assert.False(ProgramBufferSettingsSummary.RequiresRestart(3, 3));
        Assert.Equal("This app session: 3 frames requested. No restart required.",
            ProgramBufferSettingsSummary.Describe(3, 3));
    }

    [Theory]
    [InlineData(2, 2)]
    [InlineData(3, 3)]
    [InlineData(0, 3)]
    [InlineData(-1, 3)]
    [InlineData(4, 3)]
    public void SerializerRestoresOnlySupportedProgramDepth(int stored, int expected)
    {
        var preferences = ProductionOutputPreferencesSerializer.Deserialize(
            "{\"Version\":11,\"ProgramBufferFrames\":" + stored + "}");
        Assert.NotNull(preferences);
        Assert.Equal(expected, preferences.ProgramBufferFrames);
        var restored = ProductionOutputPreferencesSerializer.Deserialize(ProductionOutputPreferencesSerializer.Serialize(preferences));
        Assert.Equal(expected, restored!.ProgramBufferFrames);
    }

    [Fact]
    public void OldPreferencesDefaultToThreeAndPreserveExplicitAudioMode()
    {
        var preferences = ProductionOutputPreferencesSerializer.Deserialize(
            "{\"Version\":10,\"ZoomAudioMode\":\"programMix\"}", out var migrated);
        Assert.NotNull(preferences);
        Assert.True(migrated);
        Assert.Equal(3, preferences.ProgramBufferFrames);
        Assert.Equal("programMix", preferences.ZoomAudioMode);
    }
}
