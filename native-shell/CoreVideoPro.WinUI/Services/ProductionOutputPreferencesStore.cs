using System.Text.Json;

namespace CoreVideoPro.WinUI.Services;

public sealed class ProductionOutputPreferences
{
    // v4 (beta spec S4): StreamRtmpStreamKey / StreamSrtPassphrase are stored
    // DPAPI-encrypted at rest ("dpapi:" + base64 blob, field-level so the rest
    // of the file stays diffable). Older plaintext files load fine and are
    // re-saved encrypted on first load.
    // v5 (beta spec O1): virtual-camera enable/mirror/name persist so they
    // survive restarts (they were live-synced but reset every launch). Older
    // files migrate with the in-app defaults: enabled=false, mirror=false,
    // name=null (blank keeps the default "CoreVideo Pro Camera" name).
    // v6 (master-vst-round2-spec §B2): the master rack persists — the full
    // mastering settings block (incl. B1 glue dynamics/multiband), both A/B
    // compare slots + which slot is active, and the operator's saved presets
    // (built-ins stay code-defined in MasteringPresetCatalog and are never
    // written here). Older files migrate with null blocks = keep the in-app
    // defaults (mastering off, both slots neutral, no user presets).
    public const int CurrentVersion = 6;

    public int Version { get; set; } = CurrentVersion;
    public string? FfmpegBinDirectory { get; set; }
    public bool StreamRtmpEnabled { get; set; } = true;
    public bool StreamNdiEnabled { get; set; }
    public bool StreamSrtEnabled { get; set; }
    public string? StreamRtmpProtocol { get; set; }
    public string? StreamRtmpServerUrl { get; set; }
    public string? StreamRtmpStreamKey { get; set; }
    public string? StreamNdiProgramName { get; set; }
    public string? StreamNdiGroupName { get; set; }
    public string? StreamSrtMode { get; set; }
    public string? StreamSrtHost { get; set; }
    public string? StreamSrtPort { get; set; }
    public string? StreamSrtLatencyMs { get; set; }
    public string? StreamSrtStreamId { get; set; }
    public string? StreamSrtKeyLength { get; set; }
    public string? StreamSrtPassphrase { get; set; }
    public string? CanvasResolution { get; set; }
    public string? CanvasFps { get; set; }
    // Opening a capture endpoint can affect multi-endpoint USB interfaces such
    // as GoXLR. Capture must therefore be an explicit operator action, not a
    // side effect of launching the application.
    public bool LocalAudioSourceEnabled { get; set; }
    public string? SelectedLocalAudioCaptureDeviceId { get; set; }
    public bool AudioMonitoringEnabled { get; set; }
    public string? SelectedAudioMonitorDeviceId { get; set; }
    public double AudioMonitorVolume { get; set; } = 0.75;
    public string? StreamRenderResolution { get; set; }
    public string? StreamRenderFps { get; set; }
    public string? StreamVideoCodec { get; set; }
    public double StreamTargetBitrateMbps { get; set; }
    public int StreamAudioBitrateKbps { get; set; } = 160;
    public string? StreamEncoderMode { get; set; }
    public string? StreamPlatformProfileId { get; set; }
    public double StreamKeyframeIntervalSeconds { get; set; } = 2;
    public string? StreamRateControl { get; set; }
    public string? StreamH264Profile { get; set; }
    public int StreamBFrames { get; set; } = 2;
    public bool StreamAllowEnhancedRtmp { get; set; }
    public string? RecordingRenderResolution { get; set; }
    public string? RecordingRenderFps { get; set; }
    public string? RecordingVideoCodec { get; set; }
    public double RecordingTargetBitrateMbps { get; set; }
    public int RecordingAudioBitrateKbps { get; set; } = 192;
    public string? RecordingTargetFolder { get; set; }
    public string? RecordingFilenamePrefix { get; set; }
    public string? RecordingFormat { get; set; }
    public string? RecordingQuality { get; set; }
    public string? LowerThirdPosition { get; set; }
    public double LowerThirdBuildInMs { get; set; }
    public double LowerThirdBuildOutMs { get; set; }
    public string? BrandLowerThirdStyle { get; set; }
    public string? BrandDefaultOverlayBehavior { get; set; }
    // Virtual camera (beta spec O1): operator intent survives restarts. Enabled
    // deliberately defaults to false — exposing the program feed system-wide is
    // an explicit operator action on a fresh profile, mirroring the local-audio
    // capture posture above.
    public bool VirtualCameraEnabled { get; set; }
    public bool VirtualCameraMirror { get; set; }
    public string? VirtualCameraName { get; set; }
    public string? MultiviewLayoutMode { get; set; }
    public int MultiviewTileCount { get; set; } = 8;
    public bool MultiviewShowLabels { get; set; } = true;
    public bool MultiviewShowTally { get; set; } = true;
    public bool MultiviewShowMeters { get; set; } = true;
    public bool MultiviewShowClock { get; set; }
    public Dictionary<string, string> SceneBackgroundAssetIds { get; set; } = new(StringComparer.Ordinal);

    // Operator-overridden display names, keyed by canonical source id
    // (zoom:&lt;pid&gt; / capture:&lt;deviceId&gt; / media:&lt;assetId&gt;). When present, the
    // override replaces the derived Zoom/UVC/asset name everywhere the source is
    // labelled — most importantly the auto lower-thirds. Absent keys fall back to
    // the derived name. capture:/media: keys are stable across sessions; zoom:
    // keys are per-meeting (the SDK participant id changes), so those entries are
    // effectively session-scoped.
    public Dictionary<string, string> SourceDisplayNames { get; set; } = new(StringComparer.Ordinal);

    // Custom scenes (scenes redesign S2): previously scenes lived only in
    // process memory and died with the app. Persisted on scene lifecycle ops
    // (new/save/update/remove/duplicate) — not on every canvas drag; unsaved
    // canvas edits are committed by "Update scene", matching the save model.
    // Zoom participant ids inside routes are per-meeting and go stale across
    // sessions; show-input-slot assignments (the primary path) survive.
    public List<PersistedScene> CustomScenes { get; set; } = [];

    // Master rack (v6, master-vst-round2-spec §B2). Null blocks = an older
    // file or a fresh profile — the in-app defaults apply and nothing syncs
    // to the core beyond what the operator had before the bump.
    public PersistedMasteringSettings? MasteringCurrent { get; set; }
    public PersistedMasteringSettings? MasteringCompareA { get; set; }
    public PersistedMasteringSettings? MasteringCompareB { get; set; }
    public string MasteringCompareSlot { get; set; } = "A";
    public List<PersistedMasteringPreset> MasteringUserPresets { get; set; } = [];
}

/// <summary>
/// One complete mastering-processor state at rest (mirrors
/// <c>MasteringSettings</c>; defaults are the neutral in-app values so absent
/// JSON fields deserialize to the documented pre-B2 behavior).
/// </summary>
public sealed class PersistedMasteringSettings
{
    public bool Enabled { get; set; }
    public int TargetIndex { get; set; }
    public double GlueAmount { get; set; }
    public double CeilingDbfs { get; set; } = -1.3;
    public double MaxRideDb { get; set; }
    public double InputGainDb { get; set; }
    public double HighPassHz { get; set; }
    public double LowPassHz { get; set; }
    public double LowShelfDb { get; set; }
    public double PresenceDb { get; set; }
    public double HighShelfDb { get; set; }
    public double StereoWidth { get; set; } = 1.0;
    public bool LimiterEnabled { get; set; } = true;
    public double GlueRatio { get; set; } = 2.0;
    public double GlueAttackMs { get; set; } = 30.0;
    public double GlueReleaseMs { get; set; } = 250.0;
    public double GlueMakeupDb { get; set; }
    public bool GlueMultiband { get; set; }
    public double GlueBandLowDb { get; set; }
    public double GlueBandMidDb { get; set; }
    public double GlueBandHighDb { get; set; }
}

public sealed class PersistedMasteringPreset
{
    public string Id { get; set; } = string.Empty;
    public string Name { get; set; } = string.Empty;
    public PersistedMasteringSettings Settings { get; set; } = new();
}

public sealed class PersistedScene
{
    public string Id { get; set; } = string.Empty;
    public string Name { get; set; } = string.Empty;
    public string Layout { get; set; } = string.Empty;
    public List<PersistedSceneRoute> Routes { get; set; } = [];
}

public sealed class PersistedSceneRoute
{
    public string Id { get; set; } = string.Empty;
    public string Mode { get; set; } = "fixed";      // wire strings (SceneRoutingService)
    public string AudioRole { get; set; } = "mix";
    public string? ParticipantId { get; set; }
    public string? CaptureDeviceId { get; set; }
    public int? ShowInputSlotNumber { get; set; }
    public string? ProductionRoleId { get; set; }
    public double? RectX { get; set; }
    public double? RectY { get; set; }
    public double? RectWidth { get; set; }
    public double? RectHeight { get; set; }
    public string FitMode { get; set; } = "fill";
    public string BorderStyle { get; set; } = "accent";
    public string BorderColor { get; set; } = "#44C1A1";
    public double BorderThickness { get; set; } = 2;
    public double SourceScale { get; set; } = 1;
    public double SourceOffsetX { get; set; }
    public double SourceOffsetY { get; set; }
    public double Opacity { get; set; } = 1;
    public int ZIndex { get; set; }
}

public interface IProductionOutputPreferencesStore
{
    void Save(ProductionOutputPreferences preferences);

    ProductionOutputPreferences? Load();
}

public static class ProductionOutputPreferencesSerializer
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true
    };

    public static string Serialize(ProductionOutputPreferences preferences) =>
        JsonSerializer.Serialize(preferences, Options);

    /// <summary>
    /// Runs the two secret-bearing fields (RTMP stream key, SRT passphrase)
    /// through <paramref name="protect"/> at the JSON level so the caller's
    /// in-memory preferences object stays plaintext and every other field stays
    /// human-readable on disk (beta spec S4: field-level, not whole-file).
    /// </summary>
    public static string ProtectSecretFields(string json, Func<string, string> protect)
    {
        var node = System.Text.Json.Nodes.JsonNode.Parse(json);
        if (node is null)
        {
            return json;
        }

        foreach (var field in SecretFieldNames)
        {
            if (node[field]?.GetValue<string?>() is { Length: > 0 } value)
            {
                node[field] = protect(value);
            }
        }

        return node.ToJsonString(Options);
    }

    public static readonly string[] SecretFieldNames =
    [
        nameof(ProductionOutputPreferences.StreamRtmpStreamKey),
        nameof(ProductionOutputPreferences.StreamSrtPassphrase)
    ];

    public static ProductionOutputPreferences? Deserialize(string? json) =>
        Deserialize(json, out _);

    public static ProductionOutputPreferences? Deserialize(string? json, out bool migratedFromOlderVersion)
    {
        migratedFromOlderVersion = false;
        if (string.IsNullOrWhiteSpace(json))
        {
            return null;
        }

        try
        {
            var preferences = JsonSerializer.Deserialize<ProductionOutputPreferences>(json, Options);
            if (preferences is null)
            {
                return null;
            }

            // Versions 1-2 defaulted local capture to enabled and selected the
            // first discovered endpoint automatically. Treat that legacy state
            // as implicit, not consent to seize the device on every launch.
            // (Pinned to < 3: the v4 secrets / v5 vcam / v6 mastering bumps must
            // not re-run it. The v5 vcam and v6 mastering fields need no explicit
            // migration — absent fields deserialize to the documented defaults:
            // vcam off/unmirrored/default name; mastering blocks null = in-app
            // defaults, no user presets.)
            if (preferences.Version < 3)
            {
                preferences.LocalAudioSourceEnabled = false;
            }

            if (preferences.Version < ProductionOutputPreferences.CurrentVersion)
            {
                preferences.Version = ProductionOutputPreferences.CurrentVersion;
                migratedFromOlderVersion = true;
            }

            return preferences;
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

public sealed class FileProductionOutputPreferencesStore : IProductionOutputPreferencesStore
{
    public const string DefaultFileName = "production-output-preferences.json";

    private readonly string _filePath;
    private readonly Func<string, string>? _protectSecret;
    private readonly Func<string, string>? _unprotectSecret;

    /// <param name="protectSecret">Optional at-rest encryption for the secret
    /// fields (RTMP stream key, SRT passphrase); e.g. DPAPI via
    /// <c>DpapiSecretProtector.Protect</c>. Null keeps plaintext (tests).</param>
    /// <param name="unprotectSecret">Counterpart decryptor; must pass plaintext
    /// (unprefixed) values through unchanged so legacy files keep loading.</param>
    public FileProductionOutputPreferencesStore(
        string folderPath,
        string? fileName = null,
        Func<string, string>? protectSecret = null,
        Func<string, string>? unprotectSecret = null)
    {
        _filePath = Path.Combine(folderPath, fileName ?? DefaultFileName);
        _protectSecret = protectSecret;
        _unprotectSecret = unprotectSecret;
    }

    public void Save(ProductionOutputPreferences preferences)
    {
        var directory = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var json = ProductionOutputPreferencesSerializer.Serialize(preferences);
        if (_protectSecret is not null)
        {
            json = ProductionOutputPreferencesSerializer.ProtectSecretFields(json, _protectSecret);
        }

        File.WriteAllText(_filePath, json);
    }

    public ProductionOutputPreferences? Load()
    {
        if (!File.Exists(_filePath))
        {
            return null;
        }

        try
        {
            var preferences = ProductionOutputPreferencesSerializer.Deserialize(
                File.ReadAllText(_filePath), out var migratedFromOlderVersion);
            if (preferences is null)
            {
                return null;
            }

            var hadPlaintextSecret = false;
            if (_unprotectSecret is not null)
            {
                preferences.StreamRtmpStreamKey =
                    UnprotectField(nameof(preferences.StreamRtmpStreamKey), preferences.StreamRtmpStreamKey, ref hadPlaintextSecret);
                preferences.StreamSrtPassphrase =
                    UnprotectField(nameof(preferences.StreamSrtPassphrase), preferences.StreamSrtPassphrase, ref hadPlaintextSecret);
            }

            // Migration (beta spec S4): plaintext secrets or an older schema
            // version re-save encrypted at the new version. Best-effort — a
            // failed rewrite must never lose working preferences.
            if (_protectSecret is not null && (hadPlaintextSecret || migratedFromOlderVersion))
            {
                try
                {
                    Save(preferences);
                }
                catch (Exception ex)
                {
                    LaunchLog.Write($"prefs: encrypted re-save failed (keeping loaded preferences): {ex.Message}");
                }
            }

            return preferences;
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private string? UnprotectField(string fieldName, string? stored, ref bool hadPlaintextSecret)
    {
        if (string.IsNullOrEmpty(stored))
        {
            return stored;
        }

        try
        {
            var value = _unprotectSecret!(stored);
            // Pass-through of a non-empty value means it sat plaintext at rest.
            hadPlaintextSecret |= string.Equals(value, stored, StringComparison.Ordinal);
            return value;
        }
        catch (Exception ex)
        {
            // A blob that cannot be decrypted (e.g. the file was copied from a
            // different Windows user) is unusable — drop the single field LOUDLY
            // rather than failing the whole preferences load.
            LaunchLog.Write($"prefs: could not decrypt {fieldName}; the value must be re-entered: {ex.Message}");
            return null;
        }
    }
}

public sealed class InMemoryProductionOutputPreferencesStore : IProductionOutputPreferencesStore
{
    private ProductionOutputPreferences? _preferences;

    public void Save(ProductionOutputPreferences preferences) => _preferences = preferences;

    public ProductionOutputPreferences? Load() => _preferences;
}
