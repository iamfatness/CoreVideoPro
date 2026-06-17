namespace CoreVideoPro.WinUI;

internal static class LaunchLog
{
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CoreVideoPro",
        "launch.log");

    internal static void Write(string message) =>
        File.AppendAllText(LogPath, $"[{DateTime.Now:O}] {message}{Environment.NewLine}");
}