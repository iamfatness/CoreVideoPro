using System.Text.Json;

namespace CoreVideoPro.WinUI.Services;

public sealed class ProductionOutputPreferences
{
    public int Version { get; set; } = 1;
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
    public bool LocalAudioSourceEnabled { get; set; } = true;
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

    public static ProductionOutputPreferences? Deserialize(string? json)
    {
        if (string.IsNullOrWhiteSpace(json))
        {
            return null;
        }

        try
        {
            return JsonSerializer.Deserialize<ProductionOutputPreferences>(json, Options);
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

    public FileProductionOutputPreferencesStore(string folderPath, string? fileName = null)
    {
        _filePath = Path.Combine(folderPath, fileName ?? DefaultFileName);
    }

    public void Save(ProductionOutputPreferences preferences)
    {
        var directory = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(_filePath, ProductionOutputPreferencesSerializer.Serialize(preferences));
    }

    public ProductionOutputPreferences? Load()
    {
        if (!File.Exists(_filePath))
        {
            return null;
        }

        try
        {
            return ProductionOutputPreferencesSerializer.Deserialize(File.ReadAllText(_filePath));
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
}

public sealed class InMemoryProductionOutputPreferencesStore : IProductionOutputPreferencesStore
{
    private ProductionOutputPreferences? _preferences;

    public void Save(ProductionOutputPreferences preferences) => _preferences = preferences;

    public ProductionOutputPreferences? Load() => _preferences;
}
