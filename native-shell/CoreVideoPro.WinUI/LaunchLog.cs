using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI;

internal static class LaunchLog
{
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CoreVideoPro",
        "launch.log");

    // Best-effort logging. It is called from hot paths (including the D3D present
    // callback), so it MUST NOT throw: a locked/contended log file (another
    // instance, a tailing reader) once surfaced as an unhandled IOException out
    // of the present event handler and crashed the app. Logging failing is
    // never worth taking the studio down — swallow everything.
    internal static void Write(string message)
    {
        var line = $"[{DateTime.Now:O}] {message}{Environment.NewLine}";
        BoundedLogFile.Append(LogPath, line);
    }
}
