namespace CoreVideoPro.ShowEngine;

/// <summary>The resolved node.exe + entry script for spawning the show-engine host process,
/// plus the directory the entry script "belongs to" (the show-engine dir, one level above
/// its <c>dist\host</c> layout) and which candidate slot answered.</summary>
public sealed record ShowEngineResolvedPaths(
    string NodeExe,
    string EntryScript,
    string WorkingDirectory,
    string Source /* "env" | "packaged" | "dev" */);

/// <summary>Resolves where to find the show-engine host process (spec §6.4). Pure and
/// injectable: every environment lookup, filesystem probe, and PATH search is a parameter, so
/// tests never touch the real filesystem or environment.</summary>
public static class ShowEnginePaths
{
    public const string NodeExeEnvVar = "COREVIDEO_NODE_EXE";
    public const string ShowEngineDirEnvVar = "COREVIDEO_SHOW_ENGINE_DIR";

    /// <summary>Candidates in priority order:
    /// (1) env pair — ONLY when both <see cref="NodeExeEnvVar"/> and <see cref="ShowEngineDirEnvVar"/>
    /// are set (entry = <c>&lt;dir&gt;\dist\host\main.js</c>);
    /// (2) packaged — <c>&lt;appBaseDir&gt;\node\node.exe</c> + <c>&lt;appBaseDir&gt;\show-engine\dist\host\main.js</c>;
    /// (3) dev — <paramref name="nodeOnPath"/>() (skipped entirely when null) +
    /// <c>&lt;repoRoot&gt;\show-engine\dist\host\main.js</c>.</summary>
    public static IReadOnlyList<(string NodeExe, string EntryScript, string Source)> Candidates(
        string appBaseDir, string repoRoot, Func<string, string?> getEnv, Func<string?> nodeOnPath)
    {
        var candidates = new List<(string NodeExe, string EntryScript, string Source)>();

        var envNode = getEnv(NodeExeEnvVar);
        var envDir = getEnv(ShowEngineDirEnvVar);
        if (!string.IsNullOrWhiteSpace(envNode) && !string.IsNullOrWhiteSpace(envDir))
        {
            candidates.Add((envNode, Path.Combine(envDir, "dist", "host", "main.js"), "env"));
        }

        candidates.Add((
            Path.Combine(appBaseDir, "node", "node.exe"),
            Path.Combine(appBaseDir, "show-engine", "dist", "host", "main.js"),
            "packaged"));

        var devNode = nodeOnPath();
        if (!string.IsNullOrWhiteSpace(devNode))
        {
            candidates.Add((devNode, Path.Combine(repoRoot, "show-engine", "dist", "host", "main.js"), "dev"));
        }

        return candidates;
    }

    /// <summary>The first candidate whose BOTH files exist per <paramref name="fileExists"/>, or
    /// null when none do. <see cref="ShowEngineResolvedPaths.WorkingDirectory"/> is the
    /// show-engine directory itself — the parent of the entry script's <c>dist\host</c> layout.</summary>
    public static ShowEngineResolvedPaths? Resolve(
        Func<string, bool> fileExists, string appBaseDir, string repoRoot, Func<string, string?> getEnv, Func<string?> nodeOnPath)
    {
        foreach (var (nodeExe, entryScript, source) in Candidates(appBaseDir, repoRoot, getEnv, nodeOnPath))
        {
            if (!fileExists(nodeExe) || !fileExists(entryScript))
            {
                continue;
            }

            // entryScript = <dir>\dist\host\main.js -> host -> dist -> dir
            var hostDir = Path.GetDirectoryName(entryScript);
            var distDir = hostDir is null ? null : Path.GetDirectoryName(hostDir);
            var showEngineDir = distDir is null ? null : Path.GetDirectoryName(distDir);

            return new ShowEngineResolvedPaths(nodeExe, entryScript, showEngineDir ?? appBaseDir, source);
        }

        return null;
    }
}
