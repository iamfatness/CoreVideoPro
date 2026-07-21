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
            VirtualCameraEnabled = true,
            VirtualCameraMirror = true,
            VirtualCameraName = "Studio A Program",
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
        Assert.True(roundTripped.VirtualCameraEnabled);
        Assert.True(roundTripped.VirtualCameraMirror);
        Assert.Equal("Studio A Program", roundTripped.VirtualCameraName);
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
        // O1: fresh profiles never expose the program feed system-wide without
        // an explicit operator action; blank name keeps the built-in default.
        Assert.False(preferences.VirtualCameraEnabled);
        Assert.False(preferences.VirtualCameraMirror);
        Assert.Null(preferences.VirtualCameraName);
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
    public void Serializer_RoundTripsVstInsertStates()
    {
        // A2: persisted VST3 component states (base64) keyed by insert selection.
        var preferences = new ProductionOutputPreferences
        {
            VstInsertStates = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                ["vst:Curves AQ Stereo"] = "3q2+7wBC",  // base64 of some blob
                ["vst:TDR Nova"] = "AAECAwQ="
            }
        };

        var roundTripped = ProductionOutputPreferencesSerializer.Deserialize(
            ProductionOutputPreferencesSerializer.Serialize(preferences));

        Assert.NotNull(roundTripped);
        // The base64 blobs survive verbatim, keyed by the exact selection name.
        // (The runtime lookup in StudioViewModel rebuilds the map with an
        // OrdinalIgnoreCase comparer; System.Text.Json cannot preserve the
        // comparer through a plain-Dictionary round trip.)
        Assert.Equal("3q2+7wBC", roundTripped.VstInsertStates["vst:Curves AQ Stereo"]);
        Assert.Equal("AAECAwQ=", roundTripped.VstInsertStates["vst:TDR Nova"]);
    }

    [Fact]
    public void Serializer_MigratesOlderSchemaToV6WithEmptyVstStates()
    {
        // A v5 file (no VstInsertStates) migrates to the current version with an
        // empty map — no saved plugin state, never a crash.
        const string json = """
            {
              "Version": 5,
              "VirtualCameraEnabled": true
            }
            """;

        var migrated = ProductionOutputPreferencesSerializer.Deserialize(json, out var wasMigrated);

        Assert.NotNull(migrated);
        Assert.True(wasMigrated);
        Assert.Equal(8, ProductionOutputPreferences.CurrentVersion);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, migrated.Version);
        Assert.Empty(migrated.VstInsertStates);
        Assert.True(migrated.VirtualCameraEnabled);  // untouched fields survive
    }

    [Fact]
    public void Serializer_RoundTripsIsoRecordingSelection()
    {
        // ISO-4 (v8): the "Program + ISOs" switch + the operator's ISO source selection.
        var preferences = new ProductionOutputPreferences
        {
            IsoRecordingEnabled = true,
            IsoRecordingSourceIds = ["zoom:p2", "capture:cam0"]
        };

        var roundTripped = ProductionOutputPreferencesSerializer.Deserialize(
            ProductionOutputPreferencesSerializer.Serialize(preferences));

        Assert.NotNull(roundTripped);
        Assert.True(roundTripped.IsoRecordingEnabled);
        Assert.Equal(["zoom:p2", "capture:cam0"], roundTripped.IsoRecordingSourceIds);
    }

    [Fact]
    public void Serializer_MigratesV7FileToV8WithProgramOnlyIsoDefaults()
    {
        // A v7 file (no ISO fields) migrates to v8 = program-only: ISO off, empty
        // selection — byte-identical program-only behavior to the pre-ISO product.
        const string json = """
            {
              "Version": 7,
              "VirtualCameraEnabled": true
            }
            """;

        var migrated = ProductionOutputPreferencesSerializer.Deserialize(json, out var wasMigrated);

        Assert.NotNull(migrated);
        Assert.True(wasMigrated);
        Assert.Equal(8, ProductionOutputPreferences.CurrentVersion);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, migrated.Version);
        Assert.False(migrated.IsoRecordingEnabled);
        Assert.Empty(migrated.IsoRecordingSourceIds);
        Assert.True(migrated.VirtualCameraEnabled);  // untouched fields survive
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

    // ---- S4 secrets at rest (DPAPI field-level encryption + migration) ----

    private static FileProductionOutputPreferencesStore CreateDpapiStore(string folder) =>
        new(folder,
            protectSecret: DpapiSecretProtector.Protect,
            unprotectSecret: DpapiSecretProtector.Unprotect);

    private static string TempFolder() => Path.Combine(
        Path.GetTempPath(), "corevideo-output-preferences-tests", Guid.NewGuid().ToString("N"));

    [Fact]
    public void FileStore_WithDpapi_EncryptsOnlyTheSecretFieldsAtRest()
    {
        var folder = TempFolder();
        var store = CreateDpapiStore(folder);

        store.Save(new ProductionOutputPreferences
        {
            StreamRtmpServerUrl = "rtmps://live.example/app",
            StreamRtmpStreamKey = "super-secret-stream-key",
            StreamSrtPassphrase = "srt-passphrase-42",
            RecordingFilenamePrefix = "alpha-show"
        });

        var raw = File.ReadAllText(Path.Combine(folder, FileProductionOutputPreferencesStore.DefaultFileName));

        // Secrets are unreadable and carry the recognizable prefix...
        Assert.DoesNotContain("super-secret-stream-key", raw, StringComparison.Ordinal);
        Assert.DoesNotContain("srt-passphrase-42", raw, StringComparison.Ordinal);
        Assert.Contains(DpapiSecretProtector.Prefix, raw, StringComparison.Ordinal);
        // ...while everything else stays diffable plaintext (field-level, not whole-file).
        Assert.Contains("rtmps://live.example/app", raw, StringComparison.Ordinal);
        Assert.Contains("alpha-show", raw, StringComparison.Ordinal);

        var loaded = store.Load();
        Assert.NotNull(loaded);
        Assert.Equal("super-secret-stream-key", loaded.StreamRtmpStreamKey);
        Assert.Equal("srt-passphrase-42", loaded.StreamSrtPassphrase);
        Assert.Equal("rtmps://live.example/app", loaded.StreamRtmpServerUrl);
    }

    [Fact]
    public void FileStore_WithDpapi_MigratesPlaintextV3FileToEncryptedV4()
    {
        var folder = TempFolder();
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, FileProductionOutputPreferencesStore.DefaultFileName);
        File.WriteAllText(path, """
            {
              "Version": 3,
              "LocalAudioSourceEnabled": true,
              "StreamRtmpStreamKey": "legacy-plain-key",
              "StreamSrtPassphrase": "legacy-plain-pass"
            }
            """);

        var store = CreateDpapiStore(folder);
        var loaded = store.Load();

        // Plaintext prefs load fine (never lose a working key)...
        Assert.NotNull(loaded);
        Assert.Equal("legacy-plain-key", loaded.StreamRtmpStreamKey);
        Assert.Equal("legacy-plain-pass", loaded.StreamSrtPassphrase);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, loaded.Version);
        // ...and the v3 explicit local-audio consent is NOT reset by the v4 bump
        // (the v1-2 implicit-capture migration is pinned to < 3).
        Assert.True(loaded.LocalAudioSourceEnabled);

        // The load rewrote the file encrypted at the new version.
        var raw = File.ReadAllText(path);
        Assert.DoesNotContain("legacy-plain-key", raw, StringComparison.Ordinal);
        Assert.DoesNotContain("legacy-plain-pass", raw, StringComparison.Ordinal);
        Assert.Contains(DpapiSecretProtector.Prefix, raw, StringComparison.Ordinal);
        Assert.Contains($"\"Version\": {ProductionOutputPreferences.CurrentVersion}", raw, StringComparison.Ordinal);

        // The migrated file round-trips.
        var reloaded = store.Load();
        Assert.NotNull(reloaded);
        Assert.Equal("legacy-plain-key", reloaded.StreamRtmpStreamKey);
        Assert.Equal("legacy-plain-pass", reloaded.StreamSrtPassphrase);
    }

    // ---- O1 vcam persistence (v5 bump + v4 -> v5 migration) ----

    [Fact]
    public void FileStore_WithDpapi_MigratesV4FileToV5WithVcamDefaultsAndSecretsIntact()
    {
        var folder = TempFolder();
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, FileProductionOutputPreferencesStore.DefaultFileName);
        // A real v4 file: secrets already DPAPI-encrypted, no vcam fields yet.
        File.WriteAllText(path, $$"""
            {
              "Version": 4,
              "LocalAudioSourceEnabled": true,
              "StreamRtmpServerUrl": "rtmps://live.example/app",
              "StreamRtmpStreamKey": "{{DpapiSecretProtector.Protect("v4-stream-key")}}",
              "StreamSrtPassphrase": "{{DpapiSecretProtector.Protect("v4-srt-pass")}}",
              "RecordingTargetFolder": "D:\\Shows"
            }
            """);

        var store = CreateDpapiStore(folder);
        var loaded = store.Load();

        Assert.NotNull(loaded);
        // The v5 vcam fields land at their defaults (camera off, unmirrored,
        // default name) — a migration must never switch the camera on.
        Assert.False(loaded.VirtualCameraEnabled);
        Assert.False(loaded.VirtualCameraMirror);
        Assert.Null(loaded.VirtualCameraName);
        // Secrets and other fields survive the bump untouched...
        Assert.Equal("v4-stream-key", loaded.StreamRtmpStreamKey);
        Assert.Equal("v4-srt-pass", loaded.StreamSrtPassphrase);
        Assert.Equal("D:\\Shows", loaded.RecordingTargetFolder);
        // ...and the v4 explicit local-audio consent is NOT reset (pinned < 3).
        Assert.True(loaded.LocalAudioSourceEnabled);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, loaded.Version);

        // The load rewrote the file at v5 with the secrets still encrypted.
        var raw = File.ReadAllText(path);
        Assert.Contains($"\"Version\": {ProductionOutputPreferences.CurrentVersion}", raw, StringComparison.Ordinal);
        Assert.DoesNotContain("v4-stream-key", raw, StringComparison.Ordinal);
        Assert.DoesNotContain("v4-srt-pass", raw, StringComparison.Ordinal);
        Assert.Contains(DpapiSecretProtector.Prefix, raw, StringComparison.Ordinal);

        // The migrated file round-trips.
        var reloaded = store.Load();
        Assert.NotNull(reloaded);
        Assert.Equal("v4-stream-key", reloaded.StreamRtmpStreamKey);
        Assert.False(reloaded.VirtualCameraEnabled);
    }

    // ---- B2 master-rack persistence (v6 bump + v5 -> v6 migration) ----

    [Fact]
    public void FileStore_RoundTripsMasteringRackAndUserPresets()
    {
        var folder = TempFolder();
        var store = new FileProductionOutputPreferencesStore(folder);

        store.Save(new ProductionOutputPreferences
        {
            MasteringCurrent = new PersistedMasteringSettings
            {
                Enabled = true,
                TargetIndex = 1,
                GlueAmount = 0.45,
                CeilingDbfs = -1.0,
                MaxRideDb = 8,
                InputGainDb = 1.5,
                HighPassHz = 70,
                LowShelfDb = -1,
                PresenceDb = 1.5,
                HighShelfDb = 0.5,
                StereoWidth = 1.1,
                LimiterEnabled = true,
                GlueRatio = 3,
                GlueAttackMs = 20,
                GlueReleaseMs = 300,
                GlueMakeupDb = 1,
                GlueMultiband = true,
                GlueBandLowDb = -2,
                GlueBandMidDb = 0.5,
                GlueBandHighDb = 1
            },
            MasteringCompareA = new PersistedMasteringSettings { Enabled = true, TargetIndex = 1 },
            MasteringCompareB = new PersistedMasteringSettings { Enabled = true, TargetIndex = 2, GlueAmount = 0.3 },
            MasteringCompareSlot = "B",
            MasteringUserPresets =
            [
                new PersistedMasteringPreset
                {
                    Id = "user-abc",
                    Name = "Sunday Show",
                    Settings = new PersistedMasteringSettings { Enabled = true, GlueAmount = 0.6, GlueMultiband = true }
                }
            ]
        });

        var loaded = store.Load();

        Assert.NotNull(loaded);
        Assert.NotNull(loaded.MasteringCurrent);
        Assert.True(loaded.MasteringCurrent.Enabled);
        Assert.Equal(1, loaded.MasteringCurrent.TargetIndex);
        Assert.Equal(0.45, loaded.MasteringCurrent.GlueAmount);
        Assert.Equal(-1.0, loaded.MasteringCurrent.CeilingDbfs);
        Assert.Equal(8, loaded.MasteringCurrent.MaxRideDb);
        Assert.Equal(1.5, loaded.MasteringCurrent.InputGainDb);
        Assert.Equal(70, loaded.MasteringCurrent.HighPassHz);
        Assert.Equal(3, loaded.MasteringCurrent.GlueRatio);
        Assert.Equal(20, loaded.MasteringCurrent.GlueAttackMs);
        Assert.Equal(300, loaded.MasteringCurrent.GlueReleaseMs);
        Assert.Equal(1, loaded.MasteringCurrent.GlueMakeupDb);
        Assert.True(loaded.MasteringCurrent.GlueMultiband);
        Assert.Equal(-2, loaded.MasteringCurrent.GlueBandLowDb);
        Assert.Equal(1.1, loaded.MasteringCurrent.StereoWidth);
        Assert.Equal("B", loaded.MasteringCompareSlot);
        Assert.Equal(1, loaded.MasteringCompareA?.TargetIndex);
        Assert.Equal(2, loaded.MasteringCompareB?.TargetIndex);
        Assert.Equal(0.3, loaded.MasteringCompareB?.GlueAmount);
        var preset = Assert.Single(loaded.MasteringUserPresets);
        Assert.Equal("user-abc", preset.Id);
        Assert.Equal("Sunday Show", preset.Name);
        Assert.Equal(0.6, preset.Settings.GlueAmount);
        Assert.True(preset.Settings.GlueMultiband);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, loaded.Version);
    }

    [Fact]
    public void Serializer_MigratesV5FileToV6WithMasteringDefaults()
    {
        // A real v5 file: vcam fields present, no mastering blocks yet.
        const string json = """
            {
              "Version": 5,
              "LocalAudioSourceEnabled": true,
              "VirtualCameraEnabled": true,
              "RecordingTargetFolder": "D:\\Shows"
            }
            """;

        var migrated = ProductionOutputPreferencesSerializer.Deserialize(json, out var migratedFromOlderVersion);

        Assert.NotNull(migrated);
        Assert.True(migratedFromOlderVersion);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, migrated.Version);
        // The v6 mastering blocks land null/empty = in-app defaults; the bump
        // must never invent a rack state or flip mastering on.
        Assert.Null(migrated.MasteringCurrent);
        Assert.Null(migrated.MasteringCompareA);
        Assert.Null(migrated.MasteringCompareB);
        Assert.Equal("A", migrated.MasteringCompareSlot);
        Assert.Empty(migrated.MasteringUserPresets);
        // Earlier consents survive untouched (implicit-capture reset pinned < 3).
        Assert.True(migrated.LocalAudioSourceEnabled);
        Assert.True(migrated.VirtualCameraEnabled);
        Assert.Equal("D:\\Shows", migrated.RecordingTargetFolder);
    }

    [Fact]
    public void PersistedMasteringSettings_AbsentFieldsDeserializeToNeutralDefaults()
    {
        // A partial block (e.g. hand-edited or from a future field removal)
        // must land on the documented neutral values, not zeros that would
        // slam the ceiling to 0 dBFS or collapse the width.
        const string json = """
            {
              "Version": 6,
              "MasteringCurrent": { "Enabled": true }
            }
            """;

        var loaded = ProductionOutputPreferencesSerializer.Deserialize(json);

        Assert.NotNull(loaded?.MasteringCurrent);
        Assert.Equal(-1.3, loaded.MasteringCurrent.CeilingDbfs);
        Assert.Equal(1.0, loaded.MasteringCurrent.StereoWidth);
        Assert.True(loaded.MasteringCurrent.LimiterEnabled);
        Assert.Equal(2.0, loaded.MasteringCurrent.GlueRatio);
        Assert.Equal(30.0, loaded.MasteringCurrent.GlueAttackMs);
        Assert.Equal(250.0, loaded.MasteringCurrent.GlueReleaseMs);
        Assert.False(loaded.MasteringCurrent.GlueMultiband);
    }

    [Fact]
    public void FileStore_RoundTripsVirtualCameraIntent()
    {
        var folder = TempFolder();
        var store = new FileProductionOutputPreferencesStore(folder);

        store.Save(new ProductionOutputPreferences
        {
            VirtualCameraEnabled = true,
            VirtualCameraMirror = true,
            VirtualCameraName = "Booth Cam"
        });

        var loaded = store.Load();

        Assert.NotNull(loaded);
        Assert.True(loaded.VirtualCameraEnabled);
        Assert.True(loaded.VirtualCameraMirror);
        Assert.Equal("Booth Cam", loaded.VirtualCameraName);
        Assert.Equal(ProductionOutputPreferences.CurrentVersion, loaded.Version);
    }

    [Fact]
    public void FileStore_WithDpapi_EncryptedFileIsNotRewrittenOnLoad()
    {
        var folder = TempFolder();
        var store = CreateDpapiStore(folder);
        store.Save(new ProductionOutputPreferences { StreamRtmpStreamKey = "key-1" });

        var path = Path.Combine(folder, FileProductionOutputPreferencesStore.DefaultFileName);
        var rawBefore = File.ReadAllText(path);

        var loaded = store.Load();

        Assert.NotNull(loaded);
        Assert.Equal("key-1", loaded.StreamRtmpStreamKey);
        Assert.Equal(rawBefore, File.ReadAllText(path));
    }

    [Fact]
    public void FileStore_WithDpapi_UndecryptableSecretDropsOnlyThatField()
    {
        var folder = TempFolder();
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, FileProductionOutputPreferencesStore.DefaultFileName);
        File.WriteAllText(path, $$"""
            {
              "Version": 4,
              "StreamRtmpServerUrl": "rtmps://live.example/app",
              "StreamRtmpStreamKey": "{{DpapiSecretProtector.Prefix}}AAAA"
            }
            """);

        var store = CreateDpapiStore(folder);
        var loaded = store.Load();

        // A blob from another user/machine is unusable — the single field drops
        // (operator re-enters it) but the rest of the preferences survive.
        Assert.NotNull(loaded);
        Assert.Null(loaded.StreamRtmpStreamKey);
        Assert.Equal("rtmps://live.example/app", loaded.StreamRtmpServerUrl);
    }

    [Fact]
    public void FileStore_WithoutDelegates_KeepsPlaintextBehavior()
    {
        var folder = TempFolder();
        var store = new FileProductionOutputPreferencesStore(folder);

        store.Save(new ProductionOutputPreferences { StreamRtmpStreamKey = "plain-key" });

        var raw = File.ReadAllText(Path.Combine(folder, FileProductionOutputPreferencesStore.DefaultFileName));
        Assert.Contains("plain-key", raw, StringComparison.Ordinal);
        Assert.Equal("plain-key", store.Load()?.StreamRtmpStreamKey);
    }
}
