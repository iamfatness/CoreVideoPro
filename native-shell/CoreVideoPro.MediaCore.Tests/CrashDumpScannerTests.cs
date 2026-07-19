using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// S1 crash detection: exe-name matching, newer-than-watermark filtering, and
/// the watermark store's offer-once semantics — all against temp directories.
/// </summary>
public sealed class CrashDumpScannerTests : IDisposable
{
    private readonly string _dir = Directory.CreateTempSubdirectory("cvp-crashscan-").FullName;

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

    private string WriteDump(string name, DateTimeOffset lastWriteUtc, int length = 128)
    {
        var path = Path.Combine(_dir, name);
        File.WriteAllBytes(path, new byte[length]);
        File.SetLastWriteTimeUtc(path, lastWriteUtc.UtcDateTime);
        return path;
    }

    [Theory]
    [InlineData("corevideo-native.exe.1234.dmp", true, "corevideo-native.exe")]
    [InlineData("CoreVideoPro.WinUI.exe.999.dmp", true, "CoreVideoPro.WinUI.exe")]
    [InlineData("corevideo-zoom-engine.exe.42.dmp", true, "corevideo-zoom-engine.exe")]
    [InlineData("corevideo-browser-host.exe.7.dmp", true, "corevideo-browser-host.exe")]
    [InlineData("COREVIDEO-NATIVE.EXE.1.DMP", true, "corevideo-native.exe")] // case-insensitive
    [InlineData("corevideo-native.exe.dmp", true, "corevideo-native.exe")] // no-pid variant
    [InlineData("notepad.exe.1234.dmp", false, "")]
    [InlineData("corevideo-native.exe.1234.txt", false, "")]
    [InlineData("corevideo-native.exe", false, "")]
    [InlineData("corevideo-nativeXexe.1.dmp", false, "")]
    public void IsMonitoredDump_MatchesWerNamesForOurProcessesOnly(
        string fileName, bool expected, string expectedProcess)
    {
        var matched = CrashDumpScanner.IsMonitoredDump(fileName, out var processName);
        Assert.Equal(expected, matched);
        if (expected)
        {
            Assert.Equal(expectedProcess, processName, ignoreCase: true);
        }
    }

    [Fact]
    public void Scan_ReturnsOnlyMonitoredDumpsNewerThanWatermark_OldestFirst()
    {
        var watermark = new DateTimeOffset(2026, 7, 10, 0, 0, 0, TimeSpan.Zero);
        WriteDump("corevideo-native.exe.1.dmp", watermark - TimeSpan.FromHours(1)); // too old
        WriteDump("corevideo-native.exe.2.dmp", watermark); // exactly at watermark → seen
        var newer = WriteDump("corevideo-native.exe.3.dmp", watermark + TimeSpan.FromHours(2), length: 64);
        var newest = WriteDump("CoreVideoPro.WinUI.exe.4.dmp", watermark + TimeSpan.FromHours(3), length: 256);
        WriteDump("notepad.exe.5.dmp", watermark + TimeSpan.FromHours(4)); // not ours

        var dumps = CrashDumpScanner.Scan(_dir, watermark);

        Assert.Equal(2, dumps.Count);
        Assert.Equal(newer, dumps[0].Path);
        Assert.Equal(64, dumps[0].Length);
        Assert.Equal("corevideo-native.exe", dumps[0].ProcessName);
        Assert.Equal(newest, dumps[1].Path);
        Assert.Equal("CoreVideoPro.WinUI.exe", dumps[1].ProcessName);
        Assert.True(dumps[0].LastWriteUtc < dumps[1].LastWriteUtc);
    }

    [Fact]
    public void Scan_MissingDirectory_IsEmptyNotError()
    {
        var dumps = CrashDumpScanner.Scan(Path.Combine(_dir, "does-not-exist"), DateTimeOffset.MinValue);
        Assert.Empty(dumps);
    }

    [Fact]
    public void WatermarkStore_RoundTripsAndAppendsReports()
    {
        var path = Path.Combine(_dir, "crash-watermark.json");
        var store = new CrashReportWatermarkStore(path);

        var seen = new DateTimeOffset(2026, 7, 17, 12, 0, 0, TimeSpan.Zero);
        store.Save(new CrashReportWatermark
        {
            LastSeenUtc = seen,
            Reports =
            [
                new CrashReportRecord
                {
                    ReportId = "cv-20260717-abc",
                    SentAtUtc = seen,
                    DumpFiles = ["corevideo-native.exe.1.dmp"]
                }
            ]
        });

        var loaded = store.Load(DateTimeOffset.UtcNow);
        Assert.Equal(seen, loaded.LastSeenUtc);
        var record = Assert.Single(loaded.Reports);
        Assert.Equal("cv-20260717-abc", record.ReportId);
        Assert.Equal(["corevideo-native.exe.1.dmp"], record.DumpFiles);
    }

    [Fact]
    public void WatermarkStore_MissingFile_DefaultsToRecentLookbackNotAllHistory()
    {
        var store = new CrashReportWatermarkStore(Path.Combine(_dir, "missing.json"));
        var now = new DateTimeOffset(2026, 7, 18, 0, 0, 0, TimeSpan.Zero);
        var fresh = store.Load(now);
        // First run must not offer months-old dev dumps.
        Assert.Equal(now - CrashReportWatermarkStore.FirstRunLookback, fresh.LastSeenUtc);
        Assert.Empty(fresh.Reports);
    }

    [Fact]
    public void WatermarkStore_CorruptFile_DegradesToFreshWatermark()
    {
        var path = Path.Combine(_dir, "corrupt.json");
        File.WriteAllText(path, "{not json!!");
        var store = new CrashReportWatermarkStore(path);
        var now = DateTimeOffset.UtcNow;
        var fresh = store.Load(now);
        Assert.Equal(now - CrashReportWatermarkStore.FirstRunLookback, fresh.LastSeenUtc);
    }

    [Fact]
    public void OfferOnceFlow_AdvancingWatermarkHidesTheSameDumpNextLaunch()
    {
        var watermark = new DateTimeOffset(2026, 7, 10, 0, 0, 0, TimeSpan.Zero);
        WriteDump("corevideo-native.exe.1.dmp", watermark + TimeSpan.FromHours(1));

        var first = CrashDumpScanner.Scan(_dir, watermark);
        Assert.Single(first);

        // The launch flow advances the watermark to the newest offered dump —
        // regardless of the user's Send/Dismiss choice.
        var advanced = first.Max(d => d.LastWriteUtc);
        var second = CrashDumpScanner.Scan(_dir, advanced);
        Assert.Empty(second);
    }
}
