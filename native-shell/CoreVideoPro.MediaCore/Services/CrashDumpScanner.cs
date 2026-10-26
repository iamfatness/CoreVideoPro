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

    /// <summary>The folder the installer's WER LocalDumps keys point at when nothing else
    /// is configured: <c>%LOCALAPPDATA%\CoreVideoPro\crashes</c> (#433 / #1039).</summary>
    public static string InstallerCrashDumpDirectory() =>
        Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "crashes");

    private const string LocalDumpsKeyPath = @"SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps";

    /// <summary>
    /// Reads the raw (unexpanded) <c>DumpFolder</c> value WER LocalDumps has for
    /// <paramref name="exeName"/> from the 64-bit HKLM view, falling back to the
    /// global LocalDumps key. Null when unset or unreadable.
    /// </summary>
    public static string? ReadConfiguredDumpFolderFromRegistry(string exeName)
    {
        if (!OperatingSystem.IsWindows())
        {
            return null;
        }

        try
        {
            using var hklm = Microsoft.Win32.RegistryKey.OpenBaseKey(
                Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Registry64);
            foreach (var path in new[] { LocalDumpsKeyPath + @"\" + exeName, LocalDumpsKeyPath })
            {
                using var key = hklm.OpenSubKey(path);
                if (key?.GetValue("DumpFolder", null,
                        Microsoft.Win32.RegistryValueOptions.DoNotExpandEnvironmentNames) is string value
                    && !string.IsNullOrWhiteSpace(value))
                {
                    return value;
                }
            }
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException)
        {
            // Unreadable registry means "not configured"; the default folders still apply.
        }

        return null;
    }

    /// <summary>
    /// Every folder to scan for dumps: the per-user WER default, the configured
    /// LocalDumps <c>DumpFolder</c> of each monitored exe (environment strings
    /// expanded), and the installer default folder when no value is configured.
    /// De-duplicated case-insensitively. <paramref name="readConfiguredFolder"/>
    /// returns the raw registry value for an exe name (tests inject it).
    /// </summary>
    public static IReadOnlyList<string> ResolveDumpDirectories(
        Func<string, string?>? readConfiguredFolder = null)
    {
        readConfiguredFolder ??= ReadConfiguredDumpFolderFromRegistry;
        var folders = new List<string> { DefaultCrashDumpDirectory() };
        var anyConfigured = false;
        foreach (var exe in MonitoredExecutables)
        {
            var raw = readConfiguredFolder(exe);
            if (string.IsNullOrWhiteSpace(raw))
            {
                continue;
            }

            var expanded = Environment.ExpandEnvironmentVariables(raw.Trim());
            if (expanded.Contains('%', StringComparison.Ordinal))
            {
                continue; // unresolved variable: not a usable path
            }

            anyConfigured = true;
            folders.Add(expanded);
        }

        if (!anyConfigured)
        {
            folders.Add(InstallerCrashDumpDirectory());
        }

        return folders.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
    }

    /// <summary>Scans every directory; a dump reachable through two spellings of one folder is listed once.</summary>
    public static IReadOnlyList<CrashDumpFile> Scan(IEnumerable<string> directories, DateTimeOffset newerThanUtc)
    {
        var byPath = new Dictionary<string, CrashDumpFile>(StringComparer.OrdinalIgnoreCase);
        foreach (var directory in directories)
        {
            IReadOnlyList<CrashDumpFile> found;
            try
            {
                found = Scan(directory, newerThanUtc);
            }
            catch (Exception ex) when (ex is UnauthorizedAccessException or IOException)
            {
                // One unreadable folder (a configured DumpFolder we cannot list) must not
                // hide dumps in the others; the next launch re-offers anything still there.
                continue;
            }

            foreach (var dump in found)
            {
                byPath.TryAdd(Path.GetFullPath(dump.Path), dump);
            }
        }

        var results = byPath.Values.ToList();
        results.Sort(static (a, b) => a.LastWriteUtc.CompareTo(b.LastWriteUtc));
        return results;
    }

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
