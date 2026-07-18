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
            StreamPlatformProfileId = "youtube-1080p60",
            StreamKeyframeIntervalSeconds = 2,
            StreamRateControl = "cbr",
            StreamH264Profile = "high",
            StreamBFrames = 2,
            StreamAllowEnhancedRtmp = true,
            LocalAudioSourceEnabled = true,
            SelectedLocalAudioCaptureDeviceId = "loopback-chat",
            AudioMonitoringEnabled = true,
            SelectedAudioMonitorDeviceId = "headphones-main",
            AudioMonitorVolume = 0.62,
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
            BrandDefaultOverlayBehavior = "manual",
            MultiviewLayoutMode = "pgmPvwLarge",
            MultiviewTileCount = 5,
            MultiviewShowLabels = false,
            MultiviewShowTally = true,
            MultiviewShowMeters = false,
            MultiviewShowClock = true,
            SceneBackgroundAssetIds = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["speaker-slides"] = "media-bg-1",
                ["eight-up"] = "media-bg-2"
            },
            SourceDisplayNames = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["capture:cam-uvc"] = "Main Camera",
                ["zoom:p-42"] = "Dr. Jane Smith"
            }
        };

        var json = ProductionOutputPreferencesSerializer.Serialize(preferences);
        var roundTripped = ProductionOutputPreferencesSerializer.Deserialize(json);

        Assert.NotNull(roundTripped);
        Assert.Equal("C:\\ffmpeg\\bin", roundTripped.FfmpegBinDirectory);
        Assert.True(roundTripped.StreamNdiEnabled);
        Assert.Equal(9.5, roundTripped.StreamTargetBitrateMbps);
        Assert.Equal("nvenc", roundTripped.StreamEncoderMode);
        Assert.Equal("youtube-1080p60", roundTripped.StreamPlatformProfileId);
        Assert.Equal(2, roundTripped.StreamKeyframeIntervalSeconds);
        Assert.Equal("cbr", roundTripped.StreamRateControl);
        Assert.Equal("high", roundTripped.StreamH264Profile);
        Assert.Equal(2, roundTripped.StreamBFrames);
        Assert.True(roundTripped.StreamAllowEnhancedRtmp);
        Assert.True(roundTripped.LocalAudioSourceEnabled);
        Assert.Equal("loopback-chat", roundTripped.SelectedLocalAudioCaptureDeviceId);
        Assert.True(roundTripped.AudioMonitoringEnabled);
        Assert.Equal("headphones-main", roundTripped.SelectedAudioMonitorDeviceId);
        Assert.Equal(0.62, roundTripped.AudioMonitorVolume);
        Assert.Equal("3840x2160", roundTripped.RecordingRenderResolution);
        Assert.Equal(32, roundTripped.RecordingTargetBitrateMbps);
        Assert.Equal("mkv", roundTripped.RecordingFormat);
        Assert.Equal("archive", roundTripped.RecordingQuality);
        Assert.Equal("lower-right", roundTripped.LowerThirdPosition);
        Assert.Equal(450, roundTripped.LowerThirdBuildInMs);
        Assert.Equal(300, roundTripped.LowerThirdBuildOutMs);
        Assert.Equal("minimal", roundTripped.BrandLowerThirdStyle);
        Assert.Equal("manual", roundTripped.BrandDefaultOverlayBehavior);
        Assert.Equal("media-bg-1", roundTripped.SceneBackgroundAssetIds["speaker-slides"]);
        Assert.Equal("media-bg-2", roundTripped.SceneBackgroundAssetIds["eight-up"]);
        Assert.Equal("Main Camera", roundTripped.SourceDisplayNames["capture:cam-uvc"]);
        Assert.Equal("Dr. Jane Smith", roundTripped.SourceDisplayNames["zoom:p-42"]);
        Assert.Equal("pgmPvwLarge", roundTripped.MultiviewLayoutMode);
        Assert.Equal(5, roundTripped.MultiviewTileCount);
        Assert.False(roundTripped.MultiviewShowLabels);
        Assert.True(roundTripped.MultiviewShowTally);
        Assert.False(roundTripped.MultiviewShowMeters);
        Assert.True(roundTripped.MultiviewShowClock);
    }

    [Fact]
    public void Defaults_MatchMultiviewerContract()
    {
        var preferences = new ProductionOutputPreferences();

        // Defaults must match the fixed multiviewer contract (grid, 8 tiles, labels/tally/meters on,
        // clock off) so a fresh install starts in the documented state.
        Assert.Null(preferences.MultiviewLayoutMode);
        Assert.Equal(8, preferences.MultiviewTileCount);
        Assert.True(preferences.MultiviewShowLabels);
        Assert.True(preferences.MultiviewShowTally);
        Assert.True(preferences.MultiviewShowMeters);
        Assert.False(preferences.MultiviewShowClock);
        Assert.False(preferences.LocalAudioSourceEnabled);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, preferences.Version);
    }

    [Fact]
    public void Serializer_DisablesLegacyImplicitLocalAudioCapture()
    {
        const string json = """
            {
              "Version": 2,
              "LocalAudioSourceEnabled": true,
              "SelectedLocalAudioCaptureDeviceId": "goxlr-chat-mic"
            }
            """;

        var migrated = ProductionOutputPreferencesSerializer.Deserialize(json);

        Assert.NotNull(migrated);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, migrated.Version);
        Assert.False(migrated.LocalAudioSourceEnabled);
        Assert.Equal("goxlr-chat-mic", migrated.SelectedLocalAudioCaptureDeviceId);
    }

    [Fact]
    public void Serializer_PreservesExplicitLocalAudioCaptureInCurrentVersion()
    {
        var preferences = new ProductionOutputPreferences
        {
            LocalAudioSourceEnabled = true,
            SelectedLocalAudioCaptureDeviceId = "goxlr-chat-mic"
        };

        var roundTripped = ProductionOutputPreferencesSerializer.Deserialize(
            ProductionOutputPreferencesSerializer.Serialize(preferences));

        Assert.NotNull(roundTripped);
        Assert.True(roundTripped.LocalAudioSourceEnabled);
        Assert.Equal("goxlr-chat-mic", roundTripped.SelectedLocalAudioCaptureDeviceId);
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
            LocalAudioSourceEnabled = false,
            SelectedLocalAudioCaptureDeviceId = "loopback-game",
            AudioMonitoringEnabled = true,
            SelectedAudioMonitorDeviceId = "monitor-out",
            AudioMonitorVolume = 0.4,
            RecordingTargetBitrateMbps = 18,
            RecordingFormat = "mov",
            RecordingQuality = "standard",
            LowerThirdPosition = "upper-left",
            LowerThirdBuildInMs = 250,
            LowerThirdBuildOutMs = 200,
            SceneBackgroundAssetIds = new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["show-open"] = "media-bg-3"
            }
        });

        var loaded = store.Load();

        Assert.NotNull(loaded);
        Assert.False(loaded.StreamRtmpEnabled);
        Assert.Equal(6, loaded.StreamTargetBitrateMbps);
        Assert.False(loaded.LocalAudioSourceEnabled);
        Assert.Equal("loopback-game", loaded.SelectedLocalAudioCaptureDeviceId);
        Assert.True(loaded.AudioMonitoringEnabled);
        Assert.Equal("monitor-out", loaded.SelectedAudioMonitorDeviceId);
        Assert.Equal(0.4, loaded.AudioMonitorVolume);
        Assert.Equal(18, loaded.RecordingTargetBitrateMbps);
        Assert.Equal("mov", loaded.RecordingFormat);
        Assert.Equal("standard", loaded.RecordingQuality);
        Assert.Equal("upper-left", loaded.LowerThirdPosition);
        Assert.Equal(250, loaded.LowerThirdBuildInMs);
        Assert.Equal(200, loaded.LowerThirdBuildOutMs);
        Assert.Equal("media-bg-3", loaded.SceneBackgroundAssetIds["show-open"]);
    }

    [Fact]
    public void Serializer_ReturnsNullForInvalidJson()
    {
        var loaded = ProductionOutputPreferencesSerializer.Deserialize("{not valid");

        Assert.Null(loaded);
    }
}
