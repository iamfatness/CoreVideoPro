using CoreVideoPro.WinUI.Models;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class MasteringPresetCatalogTests
{
    [Theory]
    [InlineData("streaming", 0, -1.0)]
    [InlineData("podcast", 1, -1.0)]
    [InlineData("broadcast", 2, -1.0)]
    public void Create_ReturnsProtectedPlatformStartingPoint(string id, int targetIndex, double ceiling)
    {
        var preset = MasteringPresetCatalog.Create(id);

        Assert.True(preset.Enabled);
        Assert.True(preset.LimiterEnabled);
        Assert.Equal(targetIndex, preset.TargetIndex);
        Assert.Equal(ceiling, preset.CeilingDbfs);
        Assert.InRange(preset.GlueAmount, 0.0, 0.5);
        Assert.InRange(preset.StereoWidth, 0.0, 1.1);
    }

    [Fact]
    public void ResetStage_OnlyNeutralizesRequestedModule()
    {
        var original = MasteringPresetCatalog.Create("podcast");

        var reset = MasteringPresetCatalog.ResetStage(original, "tone");

        Assert.Equal(0.0, reset.LowShelfDb);
        Assert.Equal(0.0, reset.PresenceDb);
        Assert.Equal(0.0, reset.HighShelfDb);
        Assert.Equal(original.TargetIndex, reset.TargetIndex);
        Assert.Equal(original.GlueAmount, reset.GlueAmount);
        Assert.Equal(original.HighPassHz, reset.HighPassHz);
    }

    [Fact]
    public void ResetStage_ChainOrderIdsCoverEveryStage()
    {
        // B2 rack ids follow the DSP chain (input/filter/tone/ride/glue/width/
        // ceiling); the pre-B2 aliases keep working.
        var loaded = MasteringPresetCatalog.Create("podcast") with
        {
            InputGainDb = 2.0,
            MaxRideDb = 8.0,
            StereoWidth = 1.4,
            GlueMultiband = true,
            CeilingDbfs = -0.6
        };

        Assert.Equal(0.0, MasteringPresetCatalog.ResetStage(loaded, "ride").MaxRideDb);
        Assert.Equal(1.4, MasteringPresetCatalog.ResetStage(loaded, "ride").StereoWidth);  // ride no longer touches width
        Assert.Equal(1.0, MasteringPresetCatalog.ResetStage(loaded, "width").StereoWidth);
        Assert.Equal(8.0, MasteringPresetCatalog.ResetStage(loaded, "width").MaxRideDb);
        Assert.False(MasteringPresetCatalog.ResetStage(loaded, "glue").GlueMultiband);
        Assert.Equal(MasteringPresetCatalog.ResetStage(loaded, "glue"), MasteringPresetCatalog.ResetStage(loaded, "dynamics"));
        Assert.Equal(-1.3, MasteringPresetCatalog.ResetStage(loaded, "ceiling").CeilingDbfs);
        Assert.Equal(MasteringPresetCatalog.ResetStage(loaded, "ceiling"), MasteringPresetCatalog.ResetStage(loaded, "limiter"));
    }

    [Fact]
    public void Neutral_KeepsSafetyLimiterArmed()
    {
        var neutral = MasteringPresetCatalog.Neutral;

        Assert.True(neutral.LimiterEnabled);
        Assert.Equal(-1.3, neutral.CeilingDbfs);
        Assert.Equal(0.0, neutral.MaxRideDb);
        Assert.Equal(1.0, neutral.StereoWidth);
    }
}
