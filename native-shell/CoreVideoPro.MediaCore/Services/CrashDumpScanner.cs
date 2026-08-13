namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// A WER crash dump found in <c>%LOCALAPPDATA%\CrashDumps</c> that belongs to
/// one of the CoreVideo Pro processes (WER naming: <c>&lt;exe&gt;.&lt;pid&gt;.dmp</c>).
/// </summary>
public sealed record CrashDumpFile(
    string Path,
    string ProcessName,
    DateTimeOffset LastWriteUtc,
    long Length);

/// <summary>
/// Beta crash detection (spec docs/beta-engineering-spec.md §S1): scans the WER
/// LocalDumps folder for dumps from our processes that are newer than the
/// last-seen watermark. Pure filesystem logic — no UI, no network — so it is
/// unit-testable against a temp directory.
/// </summary>
public static class CrashDumpScanner
{
    /// <summary>The process images whose dumps we offer to report (spec §S1).</summary>
    public static readonly IReadOnlyList<string> MonitoredExecutables =
    [
        "corevideo-native.exe",
        "CoreVideoPro.WinUI.exe",
        "corevideo-zoom-engine.exe",
        "corevideo-browser-host.exe"
    ];

    /// <summary>Default WER LocalDumps location (per-user; no elevation to read).</summary>
    public static string DefaultCrashDumpDirectory() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CrashDumps");

    /// <summary>
    /// True when <paramref name="fileName"/> is a WER dump for one of our
    /// processes: <c>&lt;monitored exe&gt;.…&#8203;.dmp</c> (case-insensitive; the middle
    /// is normally the PID but is not validated — WER variants differ).
    /// </summary>
    public static bool IsMonitoredDump(string fileName, out string processName)
    {
        processName = string.Empty;
        if (!fileName.EndsWith(".dmp", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        foreach (var exe in MonitoredExecutables)
        {
            // "<exe>.dmp" or "<exe>.<pid>.dmp" — both start with "<exe>.".
            if (fileName.StartsWith(exe + ".", StringComparison.OrdinalIgnoreCase))
            {
                processName = exe;
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// Returns monitored dumps in <paramref name="directory"/> written strictly
    /// after <paramref name="newerThanUtc"/>, oldest first. A missing directory
    /// (WER LocalDumps not configured / no crash ever) is an empty result, not
    /// an error.
    /// </summary>
    public static IReadOnlyList<CrashDumpFile> Scan(string directory, DateTimeOffset newerThanUtc)
    {
        if (!Directory.Exists(directory))
        {
            return [];
        }

        var results = new List<CrashDumpFile>();
        foreach (var path in Directory.EnumerateFiles(directory, "*.dmp"))
        {
            var name = Path.GetFileName(path);
            if (!IsMonitoredDump(name, out var processName))
            {
                continue;
            }

            FileInfo info;
            try
            {
                info = new FileInfo(path);
            }
            catch (IOException)
            {
                continue; // racing WER/cleanup — skip, next launch re-offers if still there
            }

            var lastWrite = new DateTimeOffset(info.LastWriteTimeUtc, TimeSpan.Zero);
            if (lastWrite <= newerThanUtc)
            {
                continue;
            }

            results.Add(new CrashDumpFile(path, processName, lastWrite, info.Length));
        }

        results.Sort(static (a, b) => a.LastWriteUtc.CompareTo(b.LastWriteUtc));
        return results;
    }
}
