using System.IO.Compression;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// S2 support bundle v2: the export also produces a zip beside the JSON with
/// bounded, redacted log tails, a crash-dump listing (names/sizes/dates only),
/// and a manifest that records what was included vs. skipped. Missing logs must
/// never fail the export.
/// </summary>
public sealed class SupportBundleArchiveTests : IDisposable
{
    private readonly string _root = Path.Combine(
        Path.GetTempPath(), "corevideo-s2-tests", Guid.NewGuid().ToString("N"));

    public SupportBundleArchiveTests()
    {
        Directory.CreateDirectory(_root);
    }

    public void Dispose()
    {
        try
        {
            Directory.Delete(_root, recursive: true);
        }
        catch
        {
            // Temp cleanup is best-effort.
        }
    }

    private string WriteFile(string name, string content)
    {
        var path = Path.Combine(_root, name);
        File.WriteAllText(path, content, new UTF8Encoding(false));
        return path;
    }

    private static Dictionary<string, string> ReadZipEntries(string zipPath)
    {
        using var archive = ZipFile.OpenRead(zipPath);
        var entries = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in archive.Entries)
        {
            using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
            entries[entry.FullName] = reader.ReadToEnd();
        }

        return entries;
    }

    [Fact]
    public async Task Archive_ContainsManifestJsonLogsAndDumpListing()
    {
        var jsonPath = WriteFile("support-20260718.json", "{\"id\":\"support-20260718\"}");
        var launchLog = WriteFile("launch.log", "launch line 1\nlaunch line 2\n");
        var coreLog = WriteFile("media-core.log", "[core] started\n");
        var dumpsDir = Path.Combine(_root, "CrashDumps");
        Directory.CreateDirectory(dumpsDir);
        File.WriteAllBytes(Path.Combine(dumpsDir, "corevideo-native.exe.1234.dmp"), new byte[128]);

        var zipPath = SupportBundleArchiveBuilder.DefaultArchivePath(jsonPath);
        var result = await SupportBundleArchiveBuilder.WriteArchiveAsync(zipPath, jsonPath, new SupportBundleArchiveOptions
        {
            LogSources =
            [
                new SupportBundleArchiveSource { EntryName = "launch.log", Path = launchLog },
                new SupportBundleArchiveSource { EntryName = "media-core.log", Path = coreLog }
            ],
            CrashDumpsDirectory = dumpsDir
        });

        Assert.Equal(zipPath, result.ZipPath);
        Assert.True(File.Exists(zipPath));

        var entries = ReadZipEntries(zipPath);
        Assert.Contains("manifest.txt", entries.Keys);
        Assert.Contains("support-20260718.json", entries.Keys);
        Assert.Contains("logs/launch.log", entries.Keys);
        Assert.Contains("logs/media-core.log", entries.Keys);
        Assert.Contains("dumps.txt", entries.Keys);

        Assert.Contains("launch line 2", entries["logs/launch.log"]);
        Assert.Contains("support-20260718", entries["support-20260718.json"]);

        // Dump listing carries name + size, never dump content entries.
        Assert.Contains("corevideo-native.exe.1234.dmp", entries["dumps.txt"]);
        Assert.DoesNotContain(entries.Keys, key => key.EndsWith(".dmp", StringComparison.Ordinal));

        // Manifest records every included entry.
        Assert.Contains("logs/launch.log", entries["manifest.txt"]);
        Assert.Contains("[included]", entries["manifest.txt"]);
    }

    [Fact]
    public async Task MissingLogs_ProduceSkipNotesInsteadOfFailing()
    {
        var jsonPath = WriteFile("bundle.json", "{}");
        var zipPath = SupportBundleArchiveBuilder.DefaultArchivePath(jsonPath);

        var result = await SupportBundleArchiveBuilder.WriteArchiveAsync(zipPath, jsonPath, new SupportBundleArchiveOptions
        {
            LogSources =
            [
                new SupportBundleArchiveSource
                {
                    EntryName = "perf.log",
                    Path = Path.Combine(_root, "does-not-exist", "perf.log")
                }
            ],
            CrashDumpsDirectory = Path.Combine(_root, "no-such-dumps-dir")
        });

        var entries = ReadZipEntries(zipPath);
        Assert.DoesNotContain("logs/perf.log", entries.Keys);
        Assert.Contains("[skipped]", entries["manifest.txt"]);
        Assert.Contains("logs/perf.log", entries["manifest.txt"]);
        Assert.Contains("not found", entries["manifest.txt"]);

        // Missing dump dir is a note inside dumps.txt, not a failure.
        Assert.Contains("does not exist", entries["dumps.txt"]);

        var perfNote = Assert.Single(result.Entries, entry => entry.EntryName == "logs/perf.log");
        Assert.False(perfNote.Included);
    }

    [Fact]
    public void TailRead_IsBoundedForOversizedLogs()
    {
        // >2MB log: only the trailing ~2MB may land in the archive.
        var bigLogPath = Path.Combine(_root, "media-core.log");
        var line = "[core] filler line with some diagnostic text to pad the log file out\n";
        using (var writer = new StreamWriter(bigLogPath, append: false, new UTF8Encoding(false)))
        {
            var written = 0L;
            var index = 0;
            while (written < 3 * 1024 * 1024)
            {
                var stamped = $"{index++:D8} {line}";
                writer.Write(stamped);
                written += stamped.Length;
            }

            writer.Write("FINAL-MARKER-LINE\n");
        }

        var totalLength = new FileInfo(bigLogPath).Length;
        Assert.True(totalLength > SupportBundleArchiveOptions.DefaultTailByteLimit);

        var (text, reportedLength, truncated) = SupportBundleArchiveBuilder.ReadBoundedTail(
            bigLogPath, SupportBundleArchiveOptions.DefaultTailByteLimit);

        Assert.Equal(totalLength, reportedLength);
        Assert.True(truncated);
        Assert.True(
            Encoding.UTF8.GetByteCount(text) <= SupportBundleArchiveOptions.DefaultTailByteLimit,
            "tail must not exceed the byte limit");
        Assert.EndsWith("FINAL-MARKER-LINE\n", text, StringComparison.Ordinal);
        Assert.DoesNotContain("00000000 ", text, StringComparison.Ordinal); // head of file dropped
        // Tail starts on a whole line (partial first line trimmed at the cut).
        Assert.Matches(@"^\d{8} ", text);
    }

    [Fact]
    public async Task TailRead_WorksWhileWriterHoldsTheFileOpen()
    {
        // launch.log / media-core.log writers keep the file open with
        // FileShare.ReadWrite while the app runs; the tail read must not fault.
        var logPath = Path.Combine(_root, "launch.log");
        using var writer = new FileStream(logPath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite);
        var payload = Encoding.UTF8.GetBytes("concurrent line\n");
        writer.Write(payload, 0, payload.Length);
        writer.Flush();

        var jsonPath = WriteFile("bundle.json", "{}");
        var zipPath = SupportBundleArchiveBuilder.DefaultArchivePath(jsonPath);
        var result = await SupportBundleArchiveBuilder.WriteArchiveAsync(zipPath, jsonPath, new SupportBundleArchiveOptions
        {
            LogSources = [new SupportBundleArchiveSource { EntryName = "launch.log", Path = logPath }],
            CrashDumpsDirectory = null
        });

        var note = Assert.Single(result.Entries, entry => entry.EntryName == "logs/launch.log");
        Assert.True(note.Included);
        var entries = ReadZipEntries(zipPath);
        Assert.Contains("concurrent line", entries["logs/launch.log"]);
    }

    [Fact]
    public async Task LogTails_AreRedactedBeforeZipping()
    {
        var jsonPath = WriteFile("bundle.json", "{}");
        var logPath = WriteFile(
            "media-core.log",
            "[parse-dbg] FAILED len=9999 err='x' head='{\"type\":\"configure-outputs\",\"streamKey\":\"abcd-1234-SECRET\"'\n" +
            "[rtmp] connecting rtmps://a.rtmp.youtube.com/live2/abcd-1234-SECRET\n" +
            "[oauth] Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.SECRETSIG\n" +
            "[join] user_zak=zak-SECRET-value pwd=meeting-SECRET\n" +
            "[core] benign line keyframeInterval=2 status=live\n");

        var zipPath = SupportBundleArchiveBuilder.DefaultArchivePath(jsonPath);
        await SupportBundleArchiveBuilder.WriteArchiveAsync(zipPath, jsonPath, new SupportBundleArchiveOptions
        {
            LogSources = [new SupportBundleArchiveSource { EntryName = "media-core.log", Path = logPath }],
            CrashDumpsDirectory = null
        });

        var entries = ReadZipEntries(zipPath);
        var tail = entries["logs/media-core.log"];

        Assert.DoesNotContain("abcd-1234-SECRET", tail, StringComparison.Ordinal);
        Assert.DoesNotContain("zak-SECRET-value", tail, StringComparison.Ordinal);
        Assert.DoesNotContain("meeting-SECRET", tail, StringComparison.Ordinal);
        Assert.DoesNotContain("eyJhbGciOiJIUzI1NiJ9", tail, StringComparison.Ordinal);

        // Scheme/host survive so the log stays diagnosable; only the secret path goes.
        Assert.Contains("rtmps://a.rtmp.youtube.com/", tail, StringComparison.Ordinal);
        // Benign operational lines are untouched.
        Assert.Contains("keyframeInterval=2 status=live", tail, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(
        "streamKey=abc123secret done",
        "streamKey=[redacted] done")]
    [InlineData(
        "\"sdkJwt\":\"eyJx.token.sig\",\"other\":\"ok\"",
        "\"sdkJwt\":\"[redacted]\",\"other\":\"ok\"")]
    [InlineData(
        "Authorization: Bearer abc.def.ghi",
        "Authorization: Bearer [redacted]")]
    [InlineData(
        "url rtmp://ingest.example/live/streamkey123",
        "url rtmp://ingest.example/[redacted]")]
    [InlineData(
        "keyframe=2 latencyMs=41",
        "keyframe=2 latencyMs=41")]
    public void Redactor_ScrubsKnownSecretShapes(string input, string expected)
    {
        Assert.Equal(expected, SupportBundleLogRedactor.Redact(input));
    }
}
