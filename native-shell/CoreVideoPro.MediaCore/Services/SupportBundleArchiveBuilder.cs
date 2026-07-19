using System.Globalization;
using System.IO.Compression;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>A named log file to collect (as a bounded, redacted tail) into the archive.</summary>
public sealed class SupportBundleArchiveSource
{
    public required string EntryName { get; init; }
    public required string Path { get; init; }
}

public sealed class SupportBundleArchiveOptions
{
    /// <summary>~2MB per log tail. Logs can grow to multiple GB across a long
    /// show; the archive must never copy a whole log.</summary>
    public const int DefaultTailByteLimit = 2 * 1024 * 1024;

    public int TailByteLimit { get; init; } = DefaultTailByteLimit;

    public IReadOnlyList<SupportBundleArchiveSource> LogSources { get; init; } = [];

    /// <summary>Directory scanned for a names/sizes/dates listing (dumps.txt).
    /// The dumps themselves are never copied (multi-GB with DumpType=2).</summary>
    public string? CrashDumpsDirectory { get; init; }

    /// <summary>The standard shell log set: launch.log / media-core.log /
    /// perf.log (%LOCALAPPDATA%\CoreVideoPro), vcam-serve.log
    /// (%ProgramData%\CoreVideoPro), and %LOCALAPPDATA%\CrashDumps.</summary>
    public static SupportBundleArchiveOptions CreateDefault()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var appDataDir = System.IO.Path.Combine(localAppData, "CoreVideoPro");
        var programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);

        return new SupportBundleArchiveOptions
        {
            LogSources =
            [
                new SupportBundleArchiveSource { EntryName = "launch.log", Path = System.IO.Path.Combine(appDataDir, "launch.log") },
                new SupportBundleArchiveSource { EntryName = "media-core.log", Path = System.IO.Path.Combine(appDataDir, "media-core.log") },
                new SupportBundleArchiveSource { EntryName = "perf.log", Path = System.IO.Path.Combine(appDataDir, "perf.log") },
                new SupportBundleArchiveSource { EntryName = "vcam-serve.log", Path = System.IO.Path.Combine(programData, "CoreVideoPro", "vcam-serve.log") }
            ],
            CrashDumpsDirectory = System.IO.Path.Combine(localAppData, "CrashDumps")
        };
    }
}

public sealed class SupportBundleArchiveEntryNote
{
    public required string EntryName { get; init; }
    public required bool Included { get; init; }
    public required string Note { get; init; }
}

public sealed class SupportBundleArchiveResult
{
    public required string ZipPath { get; init; }
    public required IReadOnlyList<SupportBundleArchiveEntryNote> Entries { get; init; }

    public int IncludedCount => Entries.Count(entry => entry.Included);
}

/// <summary>
/// Support bundle v2: packs the redacted bundle JSON plus bounded log tails and
/// a crash-dump listing into a zip next to the JSON. Every input is optional —
/// a missing or unreadable log becomes a skip note in manifest.txt, never a
/// failed export. Log tails pass through <see cref="SupportBundleLogRedactor"/>
/// before they hit the archive.
/// </summary>
public static class SupportBundleArchiveBuilder
{
    public static string DefaultArchivePath(string bundleJsonPath) =>
        Path.ChangeExtension(bundleJsonPath, ".zip");

    public static async Task<SupportBundleArchiveResult> WriteArchiveAsync(
        string zipPath,
        string bundleJsonPath,
        SupportBundleArchiveOptions? options = null,
        CancellationToken cancellationToken = default)
    {
        options ??= SupportBundleArchiveOptions.CreateDefault();
        var notes = new List<SupportBundleArchiveEntryNote>();

        var directory = Path.GetDirectoryName(zipPath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        await using var zipStream = new FileStream(zipPath, FileMode.Create, FileAccess.Write, FileShare.Read);
        using var archive = new ZipArchive(zipStream, ZipArchiveMode.Create, leaveOpen: false);

        // 1. The redacted bundle JSON itself.
        var bundleEntryName = Path.GetFileName(bundleJsonPath);
        try
        {
            var bundleBytes = await File.ReadAllBytesAsync(bundleJsonPath, cancellationToken).ConfigureAwait(false);
            await WriteEntryAsync(archive, bundleEntryName, bundleBytes, cancellationToken).ConfigureAwait(false);
            notes.Add(new SupportBundleArchiveEntryNote
            {
                EntryName = bundleEntryName,
                Included = true,
                Note = $"included ({bundleBytes.Length} bytes, redacted support bundle JSON)"
            });
        }
        catch (Exception ex)
        {
            notes.Add(SkipNote(bundleEntryName, ex));
        }

        // 2. Bounded, redacted log tails.
        foreach (var source in options.LogSources)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var entryName = $"logs/{source.EntryName}";
            try
            {
                if (!File.Exists(source.Path))
                {
                    notes.Add(new SupportBundleArchiveEntryNote
                    {
                        EntryName = entryName,
                        Included = false,
                        Note = $"skipped: not found at {source.Path}"
                    });
                    continue;
                }

                var (tailText, totalLength, truncated) = ReadBoundedTail(source.Path, options.TailByteLimit);
                var redacted = SupportBundleLogRedactor.Redact(tailText);
                var bytes = Encoding.UTF8.GetBytes(redacted);
                await WriteEntryAsync(archive, entryName, bytes, cancellationToken).ConfigureAwait(false);
                notes.Add(new SupportBundleArchiveEntryNote
                {
                    EntryName = entryName,
                    Included = true,
                    Note = truncated
                        ? $"included (tail: last {bytes.Length} bytes of {totalLength}; redaction filter applied)"
                        : $"included (complete: {bytes.Length} bytes; redaction filter applied)"
                });
            }
            catch (Exception ex)
            {
                notes.Add(SkipNote(entryName, ex));
            }
        }

        // 3. Crash-dump listing (names/sizes/dates only — never the dumps).
        try
        {
            var dumpsText = BuildDumpListing(options.CrashDumpsDirectory);
            await WriteEntryAsync(archive, "dumps.txt", Encoding.UTF8.GetBytes(dumpsText), cancellationToken)
                .ConfigureAwait(false);
            notes.Add(new SupportBundleArchiveEntryNote
            {
                EntryName = "dumps.txt",
                Included = true,
                Note = "included (crash-dump listing: names/sizes/dates only)"
            });
        }
        catch (Exception ex)
        {
            notes.Add(SkipNote("dumps.txt", ex));
        }

        // 4. Manifest last so it reports on everything above.
        var manifest = BuildManifest(notes, options);
        await WriteEntryAsync(archive, "manifest.txt", Encoding.UTF8.GetBytes(manifest), cancellationToken)
            .ConfigureAwait(false);
        notes.Add(new SupportBundleArchiveEntryNote
        {
            EntryName = "manifest.txt",
            Included = true,
            Note = "included"
        });

        return new SupportBundleArchiveResult { ZipPath = zipPath, Entries = notes };
    }

    /// <summary>
    /// Reads at most <paramref name="tailByteLimit"/> bytes from the END of a file
    /// that may be concurrently appended to by a live process (launch.log /
    /// media-core.log writers hold FileShare.ReadWrite, and so must we). The read
    /// is bounded by the length observed at open time, so a log growing while we
    /// read cannot extend the copy. When the tail is a mid-file cut, bytes up to
    /// the first newline are dropped so the tail starts on a whole line.
    /// </summary>
    public static (string Text, long TotalLength, bool Truncated) ReadBoundedTail(string path, int tailByteLimit)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            // ReadWrite: the app is usually still appending. Delete: log rotation
            // must not fault an in-flight export.
            FileShare.ReadWrite | FileShare.Delete);

        var totalLength = stream.Length;
        var start = Math.Max(0, totalLength - tailByteLimit);
        var toRead = (int)Math.Min(tailByteLimit, totalLength - start);
        if (toRead <= 0)
        {
            return (string.Empty, totalLength, false);
        }

        stream.Seek(start, SeekOrigin.Begin);
        var buffer = new byte[toRead];
        var read = 0;
        while (read < toRead)
        {
            var chunk = stream.Read(buffer, read, toRead - read);
            if (chunk <= 0)
            {
                break; // concurrent truncation — keep what we have
            }

            read += chunk;
        }

        var offset = 0;
        if (start > 0)
        {
            // Mid-file cut: skip the partial first line.
            var newline = Array.IndexOf(buffer, (byte)'\n', 0, read);
            if (newline >= 0 && newline + 1 < read)
            {
                offset = newline + 1;
            }
        }

        var text = Encoding.UTF8.GetString(buffer, offset, read - offset);
        return (text, totalLength, start > 0);
    }

    private static string BuildDumpListing(string? crashDumpsDirectory)
    {
        var builder = new StringBuilder();
        builder.AppendLine("Recent crash dumps (listing only — dump files are NOT included in this archive).");
        if (string.IsNullOrWhiteSpace(crashDumpsDirectory))
        {
            builder.AppendLine("Crash dump directory: not configured.");
            return builder.ToString();
        }

        builder.AppendLine($"Directory: {crashDumpsDirectory}");
        if (!Directory.Exists(crashDumpsDirectory))
        {
            builder.AppendLine("Directory does not exist (no dumps recorded, or WER LocalDumps not set up).");
            return builder.ToString();
        }

        var files = new DirectoryInfo(crashDumpsDirectory)
            .EnumerateFiles()
            .OrderByDescending(file => file.LastWriteTimeUtc)
            .Take(100)
            .ToList();

        if (files.Count == 0)
        {
            builder.AppendLine("No dump files present.");
            return builder.ToString();
        }

        foreach (var file in files)
        {
            builder.AppendLine(string.Create(
                CultureInfo.InvariantCulture,
                $"{file.LastWriteTimeUtc:yyyy-MM-dd HH:mm:ss}Z  {file.Length,14:N0} bytes  {file.Name}"));
        }

        return builder.ToString();
    }

    private static string BuildManifest(IReadOnlyList<SupportBundleArchiveEntryNote> notes, SupportBundleArchiveOptions options)
    {
        var builder = new StringBuilder();
        builder.AppendLine("CoreVideo Pro support bundle archive");
        builder.AppendLine(string.Create(
            CultureInfo.InvariantCulture,
            $"Created: {DateTimeOffset.UtcNow:o}"));
        builder.AppendLine(string.Create(
            CultureInfo.InvariantCulture,
            $"Log tail limit: {options.TailByteLimit} bytes per log"));
        builder.AppendLine();
        builder.AppendLine("Redaction: the bundle JSON is redacted at build time (stream keys,");
        builder.AppendLine("passphrases, endpoint credentials/secret query params). Raw log tails");
        builder.AppendLine("additionally pass a secret-shape filter that scrubs rtmp(s) URL paths,");
        builder.AppendLine("key/token/passphrase/password/secret/pwd/zak/jwt assignments and JSON");
        builder.AppendLine("fields, Bearer values, and JWT-shaped tokens.");
        builder.AppendLine();
        builder.AppendLine("Contents:");
        foreach (var note in notes)
        {
            builder.AppendLine($"  {(note.Included ? "[included]" : "[skipped] ")} {note.EntryName} — {note.Note}");
        }

        builder.AppendLine("  [included] manifest.txt — this file");
        return builder.ToString();
    }

    private static SupportBundleArchiveEntryNote SkipNote(string entryName, Exception ex) => new()
    {
        EntryName = entryName,
        Included = false,
        Note = $"skipped: {ex.GetType().Name}: {ex.Message}"
    };

    private static async Task WriteEntryAsync(
        ZipArchive archive,
        string entryName,
        byte[] content,
        CancellationToken cancellationToken)
    {
        var entry = archive.CreateEntry(entryName, CompressionLevel.Fastest);
        await using var entryStream = entry.Open();
        await entryStream.WriteAsync(content, cancellationToken).ConfigureAwait(false);
    }
}
