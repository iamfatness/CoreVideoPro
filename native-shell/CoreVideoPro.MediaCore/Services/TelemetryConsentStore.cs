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

    // In-process serialisation of the read-modify-write cycle, plus bounded
    // retries below for another process/thread that holds the file briefly
    // (#881: a reader's FileShare.Read made the accepted-send watermark write fail).
    private static readonly object Gate = new();
    private const int IoAttempts = 20;
    private static readonly TimeSpan IoBackoff = TimeSpan.FromMilliseconds(25);

    /// <summary>Loads state; a missing/corrupt/unreadable file is a fresh, disabled default (sends nothing).</summary>
    public TelemetryConsentState Load()
    {
        try
        {
            lock (Gate)
            {
                return ReadState() ?? new TelemetryConsentState();
            }
        }
        catch
        {
            // Corrupt/locked file: default to OFF. Consent is never inferred from a bad read.
            return new TelemetryConsentState();
        }
    }

    /// <summary>
    /// Read for a read-modify-write. Unlike <see cref="Load"/> an I/O failure
    /// propagates, so a transient lock can never be turned into a persisted
    /// "disabled" default that silently revokes consent.
    /// </summary>
    private TelemetryConsentState LoadForUpdate()
    {
        try
        {
            return ReadState() ?? new TelemetryConsentState();
        }
        catch (JsonException)
        {
            return new TelemetryConsentState(); // corrupt file: rewrite as disabled default
        }
    }

    private TelemetryConsentState? ReadState()
    {
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                if (!File.Exists(_filePath))
                {
                    return null;
                }

                using var stream = new FileStream(
                    _filePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                using var reader = new StreamReader(stream, Encoding.UTF8);
                return JsonSerializer.Deserialize<TelemetryConsentState>(reader.ReadToEnd(), SerializerOptions);
            }
            catch (FileNotFoundException)
            {
                return null;
            }
            catch (IOException) when (attempt < IoAttempts)
            {
                Thread.Sleep(IoBackoff);
            }
        }
    }

    public void Save(TelemetryConsentState state)
    {
        ArgumentNullException.ThrowIfNull(state);
        lock (Gate)
        {
            WriteState(state);
        }
    }

    private void WriteState(TelemetryConsentState state)
    {
        var directory = Path.GetDirectoryName(_filePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        // Write a sibling temp file, then replace atomically, so a reader never
        // sees a truncated file (which Load() would treat as consent OFF).
        var temp = _filePath + "." + Environment.ProcessId + "." + Environment.CurrentManagedThreadId + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(state, SerializerOptions), new UTF8Encoding(false));
        try
        {
            for (var attempt = 1; ; attempt++)
            {
                try
                {
                    File.Move(temp, _filePath, overwrite: true);
                    return;
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException && attempt < IoAttempts)
                {
                    Thread.Sleep(IoBackoff);
                }
            }
        }
        finally
        {
            try { File.Delete(temp); } catch { /* best effort */ }
        }
    }

    /// <summary>Convenience: is telemetry opted in right now?</summary>
    public bool IsEnabled() => Load().Enabled;

    /// <summary>Flip the opt-in, preserving the last-sent watermark.</summary>
    public void SetEnabled(bool enabled)
    {
        lock (Gate)
        {
            var state = LoadForUpdate();
            state.Enabled = enabled;
            WriteState(state);
        }
    }

    /// <summary>Record a successful send instant (updates the crash-count delta baseline).</summary>
    public void MarkSent(DateTimeOffset sentUtc)
    {
        lock (Gate)
        {
            var state = LoadForUpdate();
            state.LastSentUtc = sentUtc;
            WriteState(state);
        }
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
