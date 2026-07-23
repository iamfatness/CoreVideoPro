using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Persisted telemetry state (spec §S3): the opt-in consent flag (default OFF)
/// plus the watermark of when we last sent, used to count crashes since the last
/// send. Deliberately a tiny STANDALONE flag file at
/// <c>%LOCALAPPDATA%\CoreVideoPro\telemetry-consent.json</c> — the SAME pattern
/// as <c>update-dismissed.json</c> / <c>crash-watermark.json</c>, chosen over a
/// ProductionOutputPreferences field so the opt-in never races the prefs
/// schema-version bumps the ISO/O1/mastering work churns, and so a fresh profile
/// (no file) sends nothing.
/// </summary>
public sealed class TelemetryConsentState
{
    /// <summary>Opt-in. Absent file or absent value = OFF — consent is never assumed.</summary>
    public bool Enabled { get; set; }

    /// <summary>When the last telemetry event was accepted by the server (for crash-count deltas).</summary>
    public DateTimeOffset? LastSentUtc { get; set; }
}

public sealed class TelemetryConsentStore
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    private readonly string _filePath;

    public TelemetryConsentStore(string filePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(filePath);
        _filePath = filePath;
    }

    public string FilePath => _filePath;

    public static string DefaultPath() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "telemetry-consent.json");

    /// <summary>Loads state; a missing/corrupt file is a fresh, disabled default (sends nothing).</summary>
    public TelemetryConsentState Load()
    {
        try
        {
            if (File.Exists(_filePath))
            {
                var parsed = JsonSerializer.Deserialize<TelemetryConsentState>(
                    File.ReadAllText(_filePath), SerializerOptions);
                if (parsed is not null)
                {
                    return parsed;
                }
            }
        }
        catch
        {
            // Corrupt/locked file: default to OFF. Consent is never inferred from a bad read.
        }

        return new TelemetryConsentState();
    }

    public void Save(TelemetryConsentState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        var directory = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        File.WriteAllText(
            _filePath,
            JsonSerializer.Serialize(state, SerializerOptions),
            new UTF8Encoding(false));
    }

    /// <summary>Convenience: is telemetry opted in right now?</summary>
    public bool IsEnabled() => Load().Enabled;

    /// <summary>Flip the opt-in, preserving the last-sent watermark.</summary>
    public void SetEnabled(bool enabled)
    {
        var state = Load();
        state.Enabled = enabled;
        Save(state);
    }

    /// <summary>Record a successful send instant (updates the crash-count delta baseline).</summary>
    public void MarkSent(DateTimeOffset sentUtc)
    {
        var state = Load();
        state.LastSentUtc = sentUtc;
        Save(state);
    }
}

/// <summary>
/// Counts crash reports recorded in the S1 crash-watermark since the last
/// telemetry send. Pure/testable: it reads the SAME
/// <see cref="CrashReportWatermark.Reports"/> the S1 pipeline appends on every
/// consented crash upload — no second crash store.
/// </summary>
public static class TelemetryCrashCount
{
    public static int CountSince(CrashReportWatermark? watermark, DateTimeOffset? lastSentUtc)
    {
        if (watermark is null)
        {
            return 0;
        }

        if (lastSentUtc is not { } since)
        {
            return watermark.Reports.Count;
        }

        return watermark.Reports.Count(report => report.SentAtUtc > since);
    }
}
