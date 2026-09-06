using System.Reflection;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Process identity shared by shell, child-pipe and performance logs.</summary>
public static class DiagnosticLog
{
    private static readonly string Session = Guid.NewGuid().ToString("N");
    private static readonly string Build = typeof(DiagnosticLog).Assembly
        .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "unknown";
    private static readonly string Identity = $"pid={Environment.ProcessId} session={Session} build={Build}";
    private static readonly DiagnosticExceptionLimiter Exceptions = new();

    // Resolve once: do not enumerate assemblies on hot render/pipe log paths.
    public static bool IsTestHost { get; } = AppDomain.CurrentDomain.GetAssemblies().Any(assembly =>
        IsTestAssembly(assembly.GetName().Name));

    public static bool IsTestAssembly(string? name) => name is "testhost" or "testhost.x86"
        || (name?.StartsWith("CoreVideoPro.", StringComparison.Ordinal) == true
            && name.EndsWith(".Tests", StringComparison.Ordinal))
        || name?.StartsWith("xunit.runner", StringComparison.OrdinalIgnoreCase) == true;

    public static string ResolvePath(string root, string fileName, bool isTestHost, string session) =>
        isTestHost ? Path.Combine(root, "test-logs", session, fileName) : Path.Combine(root, fileName);

    public static void Write(string fileName, string message)
    {
        try
        {
            var test = IsTestHost;
            var path = ResolvePath(Path.Combine(Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro"), fileName, test, Session);
            BoundedLogFile.Append(path,
                $"[{DateTimeOffset.Now:O}] [{Identity} role={(test ? "test" : "app")}] {message}{Environment.NewLine}");
        }
        catch { /* Diagnostics must never disrupt the studio. */ }
    }

    public static void WriteException(string fileName, string context, Exception exception, string? requestId = null)
    {
        try
        {
            // Do not include request IDs in the bucket key: a failing retry loop
            // produces a new ID each time. Keep the emitted request ID as evidence.
            var key = $"{fileName}|{context}|{exception.GetType().FullName}|{exception.Message}";
            if (!Exceptions.ShouldWrite(key, Environment.TickCount64, out var suppressed)) return;
            Write(fileName, $"{context} request={requestId ?? "none"} suppressedSinceLast={suppressed}: {exception}");
        }
        catch { }
    }
}

/// <summary>First error is retained, then one example plus count every ten seconds.</summary>
public sealed class DiagnosticExceptionLimiter
{
    private readonly object _gate = new();
    private readonly Dictionary<string, (long Last, int Suppressed)> _entries = new();

    public bool ShouldWrite(string key, long nowMs, out int suppressed)
    {
        lock (_gate)
        {
            if (_entries.TryGetValue(key, out var previous))
            {
                if (nowMs - previous.Last < 10_000)
                {
                    _entries[key] = (previous.Last, previous.Suppressed + 1);
                    suppressed = 0;
                    return false;
                }
                suppressed = previous.Suppressed;
            }
            else
            {
                // Bound memory even when every error carries a different message.
                if (_entries.Count >= 256) _entries.Clear();
                suppressed = 0;
            }
            _entries[key] = (nowMs, 0);
            return true;
        }
    }
}
