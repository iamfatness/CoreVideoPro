using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// On-disk watermark for crash-dump detection (spec §S1): remembers the newest
/// dump timestamp we have already offered (so each dump is offered exactly once
/// — never nag) plus the reportIds of uploads the user actually sent (so the
/// operator can reference them later).
/// </summary>
public sealed class CrashReportWatermark
{
    /// <summary>Dumps written at or before this instant have been offered already.</summary>
    public DateTimeOffset LastSeenUtc { get; set; }

    public List<CrashReportRecord> Reports { get; set; } = [];
}

public sealed class CrashReportRecord
{
    public required string ReportId { get; init; }
    public DateTimeOffset SentAtUtc { get; init; }
    public IReadOnlyList<string> DumpFiles { get; init; } = [];
}

/// <summary>
/// Loads/saves <c>%LOCALAPPDATA%\CoreVideoPro\crash-watermark.json</c>. Corrupt
/// or missing files degrade to a fresh watermark (first-run default: only offer
/// dumps from the last few days, so a dev machine's months-old dumps do not
/// greet the first beta launch).
/// </summary>
public sealed class CrashReportWatermarkStore
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    /// <summary>First-run lookback window: dumps older than this are not offered.</summary>
    public static readonly TimeSpan FirstRunLookback = TimeSpan.FromDays(3);

    private readonly string _filePath;

    public CrashReportWatermarkStore(string filePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(filePath);
        _filePath = filePath;
    }

    public string FilePath => _filePath;

    public static string DefaultPath() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "crash-watermark.json");

    public CrashReportWatermark Load() => Load(DateTimeOffset.UtcNow);

    /// <summary><paramref name="nowUtc"/> is injectable for tests.</summary>
    public CrashReportWatermark Load(DateTimeOffset nowUtc)
    {
        try
        {
            if (File.Exists(_filePath))
            {
                var json = File.ReadAllText(_filePath);
                var parsed = JsonSerializer.Deserialize<CrashReportWatermark>(json, SerializerOptions);
                if (parsed is not null)
                {
                    return parsed;
                }
            }
        }
        catch
        {
            // Corrupt/locked watermark: fall through to the fresh default. Offering a
            // recent dump twice is acceptable; crashing the crash reporter is not.
        }

        return new CrashReportWatermark { LastSeenUtc = nowUtc - FirstRunLookback };
    }

    public void Save(CrashReportWatermark watermark)
    {
        ArgumentNullException.ThrowIfNull(watermark);
        var directory = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var json = JsonSerializer.Serialize(watermark, SerializerOptions);
        File.WriteAllText(_filePath, json, new UTF8Encoding(false));
    }
}
