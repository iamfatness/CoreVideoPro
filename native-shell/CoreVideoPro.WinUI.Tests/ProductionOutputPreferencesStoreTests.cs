using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ProductionOutputPreferencesStoreTests
{
    [Fact]
    public void Serializer_RoundTripsStreamAndRecordingOutputControls()
    {
        var preferences = new ProductionOutputPreferences
        {
            FfmpegBinDirectory = "C:\\ffmpeg\\bin",
            StreamRtmpEnabled = true,
            StreamNdiEnabled = true,
            StreamSrtEnabled = false,
            StreamRtmpProtocol = "rtmps",
            StreamRtmpServerUrl = "rtmps://live.example/app",
            StreamRtmpStreamKey = "secret",
            StreamNdiProgramName = "CoreVideo Program",
            StreamNdiGroupName = "public",
            StreamRenderResolution = "1920x1080",
            StreamRenderFps = "60",
            StreamVideoCodec = "h264",
            StreamTargetBitrateMbps = 9.5,
            StreamEncoderMode = "nvenc",
            RecordingRenderResolution = "3840x2160",
            RecordingRenderFps = "30",
            RecordingVideoCodec = "h265",
            RecordingTargetBitrateMbps = 32,
            RecordingTargetFolder = "D:\\Shows",
            RecordingFilenamePrefix = "alpha-show",
            RecordingFormat = "mkv",
            RecordingQuality = "archive",
            LowerThirdPosition = "lower-right",
            LowerThirdBuildInMs = 450,
            LowerThirdBuildOutMs = 300,
            BrandLowerThirdStyle = "minimal",
            BrandDefaultOverlayBehavior = "manual"
        };

        var json = ProductionOutputPreferencesSerializer.Serialize(preferences);
        var roundTripped = ProductionOutputPreferencesSerializer.Deserialize(json);

        Assert.NotNull(roundTripped);
        Assert.Equal("C:\\ffmpeg\\bin", roundTripped.FfmpegBinDirectory);
        Assert.True(roundTripped.StreamNdiEnabled);
        Assert.Equal(9.5, roundTripped.StreamTargetBitrateMbps);
        Assert.Equal("nvenc", roundTripped.StreamEncoderMode);
        Assert.Equal("3840x2160", roundTripped.RecordingRenderResolution);
        Assert.Equal(32, roundTripped.RecordingTargetBitrateMbps);
        Assert.Equal("mkv", roundTripped.RecordingFormat);
        Assert.Equal("archive", roundTripped.RecordingQuality);
        Assert.Equal("lower-right", roundTripped.LowerThirdPosition);
        Assert.Equal(450, roundTripped.LowerThirdBuildInMs);
        Assert.Equal(300, roundTripped.LowerThirdBuildOutMs);
        Assert.Equal("minimal", roundTripped.BrandLowerThirdStyle);
        Assert.Equal("manual", roundTripped.BrandDefaultOverlayBehavior);
    }

    [Fact]
    public void FileStore_RoundTripsOutputPreferences()
    {
        var folder = Path.Combine(Path.GetTempPath(), "corevideo-output-preferences-tests", Guid.NewGuid().ToString("N"));
        var store = new FileProductionOutputPreferencesStore(folder);

        store.Save(new ProductionOutputPreferences
        {
            StreamRtmpEnabled = false,
            StreamTargetBitrateMbps = 6,
            RecordingTargetBitrateMbps = 18,
            RecordingFormat = "mov",
            RecordingQuality = "standard",
            LowerThirdPosition = "upper-left",
            LowerThirdBuildInMs = 250,
            LowerThirdBuildOutMs = 200
        });

        var loaded = store.Load();

        Assert.NotNull(loaded);
        Assert.False(loaded.StreamRtmpEnabled);
        Assert.Equal(6, loaded.StreamTargetBitrateMbps);
        Assert.Equal(18, loaded.RecordingTargetBitrateMbps);
        Assert.Equal("mov", loaded.RecordingFormat);
        Assert.Equal("standard", loaded.RecordingQuality);
        Assert.Equal("upper-left", loaded.LowerThirdPosition);
        Assert.Equal(250, loaded.LowerThirdBuildInMs);
        Assert.Equal(200, loaded.LowerThirdBuildOutMs);
    }

    [Fact]
    public void Serializer_ReturnsNullForInvalidJson()
    {
        var loaded = ProductionOutputPreferencesSerializer.Deserialize("{not valid");

        Assert.Null(loaded);
    }
}
