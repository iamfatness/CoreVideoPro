using System.IO.Compression;
using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// S1 report assembly: bounded log tails, the ~24MB size cap with
/// largest-that-fits dump selection, and a manifest that names everything
/// included or skipped (the consent rule requires the zip to be inspectable).
/// </summary>
public sealed class CrashReportArchiveBuilderTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cvp-crashzip-").FullName;

    public void Dispose()
    {
        try
        {
            Directory.Delete(_dir, recursive: true);
        }
        catch
        {
            // best effort
        }
    }

    private CrashDumpFile MakeDump(string name, int length, DateTimeOffset? lastWrite = null)
    {
        var path = Path.Combine(_dir, name);
        var bytes = new byte[length];
        Random.Shared.NextBytes(bytes); // incompressible-ish, exercises real zip IO
        File.WriteAllBytes(path, bytes);
        var stamp = lastWrite ?? DateTimeOffset.UtcNow;
        File.SetLastWriteTimeUtc(path, stamp.UtcDateTime);
        return new CrashDumpFile(path, "corevideo-native.exe", stamp, length);
    }

    private string WriteLog(string name, byte[] content)
    {
        var path = Path.Combine(_dir, name);
        File.WriteAllBytes(path, content);
        return path;
    }

    private static Dictionary<string, byte[]> ReadZip(string zipPath)
    {
        var entries = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        using var zip = ZipFile.OpenRead(zipPath);
        foreach (var entry in zip.Entries)
        {
            using var stream = entry.Open();
            using var buffer = new MemoryStream();
            stream.CopyTo(buffer);
            entries[entry.FullName] = buffer.ToArray();
        }

        return entries;
    }

    [Fact]
    public void Build_IncludesDumpLogTailsBundleAndManifest()
    {
        var dump = MakeDump("corevideo-native.exe.100.dmp", 4096);
        var launchLog = WriteLog("launch.log", Encoding.UTF8.GetBytes("launch line 1\nlaunch line 2\n"));
        var bundlePath = WriteLog("support-bundle.json", Encoding.UTF8.GetBytes("{\"id\":\"support-x\"}"));
        var zipPath = Path.Combine(_dir, "out", "report.zip");

        var result = CrashReportArchiveBuilder.Build(zipPath, new CrashReportArchiveRequest
        {
            Dumps = [dump],
            LogFiles = [launchLog, Path.Combine(_dir, "media-core.log") /* missing */],
            SupportBundlePath = bundlePath
        });

        Assert.True(File.Exists(zipPath));
        Assert.True(result.ContainsDump);
        var entries = ReadZip(zipPath);
        Assert.Contains("manifest.txt", entries.Keys);
        Assert.Contains("dumps/corevideo-native.exe.100.dmp", entries.Keys);
        Assert.Contains("logs/launch.log", entries.Keys);
        Assert.Contains("support-bundle.json", entries.Keys);
        Assert.Equal(File.ReadAllBytes(dump.Path), entries["dumps/corevideo-native.exe.100.dmp"]);

        var manifest = Encoding.UTF8.GetString(entries["manifest.txt"]);
        Assert.Contains("Included: dumps/corevideo-native.exe.100.dmp", manifest);
        Assert.Contains("Included: logs/launch.log", manifest);
        Assert.Contains("Included: support-bundle.json", manifest);
        Assert.Contains("Skipped: media-core.log — not found", manifest);
    }

    [Fact]
    public void Build_BoundsLogTailsToTheConfiguredBytes_KeepingTheEnd()
    {
        // 3KB log, 1KB tail bound → the LAST 1KB must land in the zip.
        var content = new byte[3 * 1024];
        for (var i = 0; i < content.Length; i++)
        {
            content[i] = (byte)(i % 251);
        }

        var logPath = WriteLog("launch.log", content);
        var zipPath = Path.Combine(_dir, "tail.zip");

        var result = CrashReportArchiveBuilder.Build(zipPath, new CrashReportArchiveRequest
        {
            Dumps = [],
            LogFiles = [logPath],
            LogTailBytes = 1024
        });

        var entries = ReadZip(zipPath);
        var tail = entries["logs/launch.log"];
        Assert.Equal(1024, tail.Length);
        Assert.Equal(content[^1024..], tail);
        Assert.Contains("last 1024 bytes of a 3072-byte file", result.ManifestText);
    }

    [Fact]
    public void ReadTailBounded_WorksWhileAnotherWriterHoldsTheFileOpen()
    {
        var path = Path.Combine(_dir, "held-open.log");
        using var writer = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.ReadWrite);
        writer.Write("held-open content"u8);
        writer.Flush();

        // LaunchLog keeps the file open with FileShare.ReadWrite; the tail read
        // must succeed anyway.
        var (bytes, originalLength) = CrashReportArchiveBuilder.ReadTailBounded(path, 1024);
        Assert.Equal("held-open content", Encoding.UTF8.GetString(bytes));
        Assert.Equal(17, originalLength);
    }

    [Fact]
    public void Build_EnforcesTheSizeCap_IncludingLargestThatFits()
    {
        // Cap of 300KB (test-sized). Reserve slack is 64KB → dump budget ≈ 236KB.
        // Dumps: 200KB (fits), 150KB (no longer fits), 30KB (fits after).
        var big = MakeDump("corevideo-native.exe.1.dmp", 200 * 1024);
        var mid = MakeDump("corevideo-native.exe.2.dmp", 150 * 1024);
        var small = MakeDump("corevideo-native.exe.3.dmp", 30 * 1024);
        var zipPath = Path.Combine(_dir, "capped.zip");

        var result = CrashReportArchiveBuilder.Build(zipPath, new CrashReportArchiveRequest
        {
            Dumps = [big, mid, small],
            MaxTotalBytes = 300 * 1024
        });

        var entries = ReadZip(zipPath);
        Assert.Contains("dumps/corevideo-native.exe.1.dmp", entries.Keys);
        Assert.DoesNotContain("dumps/corevideo-native.exe.2.dmp", entries.Keys);
        Assert.Contains("dumps/corevideo-native.exe.3.dmp", entries.Keys);
        Assert.Contains(result.SkippedEntries, s => s.Contains("corevideo-native.exe.2.dmp"));
        Assert.Contains("Skipped: corevideo-native.exe.2.dmp", result.ManifestText);
    }

    [Fact]
    public void Build_SingleOversizedDump_IsSkippedAndNotedLoudly()
    {
        var huge = MakeDump("corevideo-native.exe.9.dmp", 512 * 1024);
        var zipPath = Path.Combine(_dir, "oversized.zip");

        var result = CrashReportArchiveBuilder.Build(zipPath, new CrashReportArchiveRequest
        {
            Dumps = [huge],
            MaxTotalBytes = 256 * 1024
        });

        Assert.False(result.ContainsDump);
        var entries = ReadZip(zipPath);
        Assert.Contains("manifest.txt", entries.Keys);
        Assert.Contains("Skipped: corevideo-native.exe.9.dmp", result.ManifestText);
        Assert.Contains("exceeds the remaining budget", result.ManifestText);
    }

    [Fact]
    public void Build_ZipStaysUnderTheUncompressedBudget()
    {
        // Random (incompressible) content proves the cap holds even worst-case.
        var dump = MakeDump("corevideo-native.exe.5.dmp", 100 * 1024);
        var zipPath = Path.Combine(_dir, "budget.zip");

        var result = CrashReportArchiveBuilder.Build(zipPath, new CrashReportArchiveRequest
        {
            Dumps = [dump],
            MaxTotalBytes = 200 * 1024
        });

        // Deflate overhead on incompressible data is bounded; the budget already
        // reserves 64KB slack for the manifest + entry headers.
        Assert.True(result.ZipLength <= 200 * 1024, $"zip is {result.ZipLength} bytes");
    }
}
