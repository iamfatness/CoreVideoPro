namespace CoreVideoPro.MediaCore.Services;

public static class MediaCorePaths
{
    public static string RepoRoot
    {
        get
        {
            var dir = AppContext.BaseDirectory;
            for (var i = 0; i < 8; i++)
            {
                if (File.Exists(Path.Combine(dir, "package.json")) &&
                    Directory.Exists(Path.Combine(dir, "native")) &&
                    Directory.Exists(Path.Combine(dir, "desktop")))
                {
                    return dir;
                }

                var parent = Directory.GetParent(dir);
                if (parent is null)
                {
                    break;
                }

                dir = parent.FullName;
            }

            // Packaged demo layout (artifacts/native/win-unpacked) has no repo markers.
            return AppContext.BaseDirectory;
        }
    }

    public static string? ResolveNativeCoreExecutable()
    {
        var repo = RepoRoot;
        var candidates = new[]
        {
            Path.Combine(repo, "native", "build-dev", "corevideo-native.exe"),
            Path.Combine(repo, "native", "build", "corevideo-native.exe"),
            Path.Combine(repo, "native", "build-dev", "Release", "corevideo-native.exe"),
            Path.Combine(AppContext.BaseDirectory, "corevideo-native.exe")
        };

        return candidates.FirstOrDefault(File.Exists);
    }

    public static string? ResolveZoomSdkArchitectureRoot()
    {
        var repo = RepoRoot;
        var envPath = Environment.GetEnvironmentVariable("ZOOM_SDK_DIR");
        var candidates = new List<string>();
        if (!string.IsNullOrWhiteSpace(envPath))
        {
            candidates.Add(envPath);
        }

        candidates.AddRange(
        [
            Path.Combine(repo, "native-core", "zoom-runtime", "windows", "x64"),
            Path.Combine(repo, "native", "build-dev"),
            Path.Combine(repo, "native", "build"),
            Path.Combine(AppContext.BaseDirectory, "zoom-runtime", "windows", "x64"),
            Path.Combine(AppContext.BaseDirectory)
        ]);

        foreach (var candidate in candidates)
        {
            if (string.IsNullOrWhiteSpace(candidate) || !Directory.Exists(candidate))
            {
                continue;
            }

            var sdkDll = Path.Combine(candidate, "bin", "sdk.dll");
            if (File.Exists(sdkDll))
            {
                return candidate;
            }

            if (File.Exists(Path.Combine(candidate, "sdk.dll")))
            {
                return candidate;
            }
        }

        return null;
    }

    public static string? ResolveNodeCoreStub()
    {
        var appRoot = RepoRoot;
        var stubCandidates = new[]
        {
            Path.Combine(appRoot, "desktop", "coreStub.cjs"),
            Path.Combine(AppContext.BaseDirectory, "desktop", "coreStub.cjs")
        };

        if (stubCandidates.All(path => !File.Exists(path)))
        {
            return null;
        }

        var electron = Path.Combine(appRoot, "node_modules", "electron", "dist", "electron.exe");
        return File.Exists(electron) ? electron : "node";
    }
}