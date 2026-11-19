namespace CoreVideoPro.MediaCore.Tests;

/// <summary>The repository root, for the shared control fixtures (contracts/control.fixtures.json).
/// A copy of CoreVideoPro.Control.Tests' RepoPaths, deliberately not a project reference.</summary>
internal static class ControlRepoPaths
{
    public static string Root { get; } = Find();

    private static string Find()
    {
        var start = AppContext.BaseDirectory;
        for (var dir = new DirectoryInfo(start); dir is not null; dir = dir.Parent)
        {
            if (File.Exists(Path.Combine(dir.FullName, "contracts", "README.md"))) return dir.FullName;
        }
        throw new InvalidOperationException($"Repository root (contracts/README.md) not found above {start}.");
    }

    public static string Contract(string name) => Path.Combine(Root, "contracts", name);
}
