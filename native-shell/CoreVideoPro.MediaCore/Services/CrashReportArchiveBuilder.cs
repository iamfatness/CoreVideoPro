using System.Globalization;
using System.IO.Compression;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Inputs for one crash-report zip (spec §S1).</summary>
public sealed class CrashReportArchiveRequest
{
    /// <summary>Dumps to include (subject to the size budget).</summary>
    public required IReadOnlyList<CrashDumpFile> Dumps { get; init; }

    /// <summary>
    /// Log files to include as bounded tails (missing files are noted as
    /// skipped, not errors): launch.log, media-core.log, perf.log.
    /// </summary>
    public IReadOnlyList<string> LogFiles { get; init; } = [];

    /// <summary>Path of a freshly exported (already REDACTED) support-bundle JSON.</summary>
    public string? SupportBundlePath { get; init; }

    /// <summary>
    /// Uncompressed-content budget. Default 24MB leaves headroom under the
    /// ingest worker's 25MB body cap (deflate only shrinks dump/log content).
    /// </summary>
    public long MaxTotalBytes { get; init; } = 24L * 1024 * 1024;

    /// <summary>Per-log tail bound (~2MB).</summary>
    public long LogTailBytes { get; init; } = 2L * 1024 * 1024;

    /// <summary>App version stamped into manifest.txt.</summary>
    public string AppVersion { get; init; } = new SupportBundleAppInfo().Version;
}

public sealed class CrashReportArchiveResult
{
    public required string ZipPath { get; init; }
    public required long ZipLength { get; init; }
    public required IReadOnlyList<string> IncludedEntries { get; init; }
    public required IReadOnlyList<string> SkippedEntries { get; init; }
    public required string ManifestText { get; init; }

    /// <summary>True when at least one crash dump made it into the archive.</summary>
    public bool ContainsDump => IncludedEntries.Any(static e => e.StartsWith("dumps/", StringComparison.Ordinal));
}

/// <summary>
/// Assembles the crash-report zip: dump(s) + bounded log tails + the redacted
/// support-bundle JSON + a manifest.txt describing exactly what was included
/// and what was skipped (consent rule: the operator can inspect the zip on
/// disk before sending). Pure file/zip logic — unit-tested against temp dirs.
/// </summary>
public static class CrashReportArchiveBuilder
{
    public static CrashReportArchiveResult Build(string zipPath, CrashReportArchiveRequest request)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(zipPath);
        ArgumentNullException.ThrowIfNull(request);

        var included = new List<string>();
        var skipped = new List<string>();
        var manifest = new StringBuilder();
        manifest.AppendLine("CoreVideo Pro crash report");
        manifest.AppendLine($"Created: {DateTimeOffset.UtcNow.UtcDateTime.ToString("o", CultureInfo.InvariantCulture)}");
        manifest.AppendLine($"App version: {request.AppVersion}");
        manifest.AppendLine($"Content budget: {request.MaxTotalBytes} bytes (uncompressed)");
        manifest.AppendLine();

        // Gather the small payloads first so the dump budget is what remains.
        var logTails = new List<(string EntryName, byte[] Bytes, long OriginalLength, bool Truncated)>();
        foreach (var logPath in request.LogFiles)
        {
            var name = Path.GetFileName(logPath);
            if (!File.Exists(logPath))
            {
                skipped.Add($"{name} — not found at {logPath}");
                continue;
            }

            try
            {
                var (bytes, originalLength) = ReadTailBounded(logPath, request.LogTailBytes);
                logTails.Add(($"logs/{name}", bytes, originalLength, originalLength > bytes.LongLength));
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                skipped.Add($"{name} — unreadable ({ex.GetType().Name}: {ex.Message})");
            }
        }

        byte[]? bundleBytes = null;
        var bundleEntryName = "support-bundle.json";
        if (!string.IsNullOrWhiteSpace(request.SupportBundlePath))
        {
            if (File.Exists(request.SupportBundlePath))
            {
                try
                {
                    bundleBytes = File.ReadAllBytes(request.SupportBundlePath);
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    skipped.Add($"{bundleEntryName} — unreadable ({ex.GetType().Name}: {ex.Message})");
                }
            }
            else
            {
                skipped.Add($"{bundleEntryName} — support bundle export produced no file");
            }
        }
        else
        {
            skipped.Add($"{bundleEntryName} — support bundle export unavailable this session");
        }

        var reserved = logTails.Sum(static t => t.Bytes.LongLength) + (bundleBytes?.LongLength ?? 0);
        var dumpBudget = Math.Max(0, request.MaxTotalBytes - reserved - 64 * 1024 /* manifest slack */);

        // Largest-first greedy: when everything fits, everything is included; when
        // it does not, we prefer the biggest dumps that fit (spec: "include the
        // largest-that-fits and note the skip").
        var dumpPlan = new List<CrashDumpFile>();
        var remaining = dumpBudget;
        foreach (var dump in request.Dumps.OrderByDescending(static d => d.Length))
        {
            if (dump.Length <= remaining)
            {
                dumpPlan.Add(dump);
                remaining -= dump.Length;
            }
            else
            {
                skipped.Add(
                    $"{Path.GetFileName(dump.Path)} — {dump.Length} bytes exceeds the remaining budget " +
                    $"({remaining} of {dumpBudget} dump bytes left; total cap {request.MaxTotalBytes})");
            }
        }

        // Stable order inside the zip: oldest dump first (matches scanner order).
        dumpPlan.Sort(static (a, b) => a.LastWriteUtc.CompareTo(b.LastWriteUtc));

        var directory = Path.GetDirectoryName(zipPath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        using (var stream = new FileStream(zipPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None))
        using (var zip = new ZipArchive(stream, ZipArchiveMode.Create, leaveOpen: false, Encoding.UTF8))
        {
            foreach (var dump in dumpPlan)
            {
                var entryName = $"dumps/{Path.GetFileName(dump.Path)}";
                try
                {
                    var entry = zip.CreateEntry(entryName, CompressionLevel.Optimal);
                    entry.LastWriteTime = dump.LastWriteUtc;
                    using var target = entry.Open();
                    // ReadWrite|Delete share: WER/cleanup may still hold the file.
                    using var source = new FileStream(
                        dump.Path, FileMode.Open, FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete);
                    source.CopyTo(target);
                    included.Add(entryName);
                    manifest.AppendLine(
                        $"Included: {entryName} ({dump.Length} bytes, {dump.ProcessName}, " +
                        $"last write {dump.LastWriteUtc.UtcDateTime.ToString("o", CultureInfo.InvariantCulture)})");
                }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
                {
                    skipped.Add($"{Path.GetFileName(dump.Path)} — unreadable ({ex.GetType().Name}: {ex.Message})");
                }
            }

            foreach (var (entryName, bytes, originalLength, truncated) in logTails)
            {
                WriteEntry(zip, entryName, bytes);
                included.Add(entryName);
                manifest.AppendLine(truncated
                    ? $"Included: {entryName} (last {bytes.LongLength} bytes of a {originalLength}-byte file)"
                    : $"Included: {entryName} ({bytes.LongLength} bytes, complete)");
            }

            if (bundleBytes is not null)
            {
                WriteEntry(zip, bundleEntryName, bundleBytes);
                included.Add(bundleEntryName);
                manifest.AppendLine($"Included: {bundleEntryName} ({bundleBytes.LongLength} bytes, redacted diagnostics snapshot)");
            }

            if (skipped.Count > 0)
            {
                manifest.AppendLine();
                foreach (var reason in skipped)
                {
                    manifest.AppendLine($"Skipped: {reason}");
                }
            }

            var manifestText = manifest.ToString();
            WriteEntry(zip, "manifest.txt", Encoding.UTF8.GetBytes(manifestText));
            included.Insert(0, "manifest.txt");
        }

        return new CrashReportArchiveResult
        {
            ZipPath = zipPath,
            ZipLength = new FileInfo(zipPath).Length,
            IncludedEntries = included,
            SkippedEntries = skipped,
            ManifestText = manifest.ToString()
        };
    }

    /// <summary>
    /// Reads at most the last <paramref name="maxBytes"/> bytes of a file that
    /// may still be actively written/held open (FileShare.ReadWrite — the same
    /// posture LaunchLog uses on the writer side).
    /// </summary>
    public static (byte[] Bytes, long OriginalLength) ReadTailBounded(string path, long maxBytes)
    {
        using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var length = stream.Length;
        var toRead = (int)Math.Min(length, maxBytes);
        if (toRead <= 0)
        {
            return ([], length);
        }

        stream.Seek(length - toRead, SeekOrigin.Begin);
        var buffer = new byte[toRead];
        stream.ReadExactly(buffer, 0, toRead);
        return (buffer, length);
    }

    private static void WriteEntry(ZipArchive zip, string entryName, byte[] bytes)
    {
        var entry = zip.CreateEntry(entryName, CompressionLevel.Optimal);
        using var target = entry.Open();
        target.Write(bytes, 0, bytes.Length);
    }
}
