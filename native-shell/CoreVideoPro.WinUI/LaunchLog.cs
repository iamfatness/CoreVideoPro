using System.Text;

namespace CoreVideoPro.WinUI;

internal static class LaunchLog
{
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CoreVideoPro",
        "launch.log");

    // Serializes writers within this process; FileShare.ReadWrite lets other
    // instances and log readers hold the file at the same time.
    private static readonly object Gate = new();

    // Best-effort logging. It is called from hot paths (including the D3D present
    // callback), so it MUST NOT throw: a locked/contended log file (another
    // instance, a tailing reader) once surfaced as an unhandled IOException out
    // of the present event handler and crashed the app. Logging failing is
    // never worth taking the studio down — swallow everything.
    internal static void Write(string message)
    {
        var line = $"[{DateTime.Now:O}] {message}{Environment.NewLine}";
        try
        {
            lock (Gate)
            {
                using var stream = new FileStream(
                    LogPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                var bytes = Encoding.UTF8.GetBytes(line);
                stream.Write(bytes, 0, bytes.Length);
            }
        }
        catch
        {
            // Logging is best-effort; never let a log write fault reach the caller.
        }
    }
}
