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
    public string? StreamRenderResolution { get; set; }
    public string? StreamRenderFps { get; set; }
    public string? StreamVideoCodec { get; set; }
    public double StreamTargetBitrateMbps { get; set; }
    public string? StreamEncoderMode { get; set; }
    public string? RecordingRenderResolution { get; set; }
    public string? RecordingRenderFps { get; set; }
    public string? RecordingVideoCodec { get; set; }
    public double RecordingTargetBitrateMbps { get; set; }
    public string? RecordingTargetFolder { get; set; }
    public string? RecordingFilenamePrefix { get; set; }
    public string? RecordingFormat { get; set; }
    public string? RecordingQuality { get; set; }
    public string? LowerThirdPosition { get; set; }
    public double LowerThirdBuildInMs { get; set; }
    public double LowerThirdBuildOutMs { get; set; }
    public string? BrandLowerThirdStyle { get; set; }
    public string? BrandDefaultOverlayBehavior { get; set; }
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
