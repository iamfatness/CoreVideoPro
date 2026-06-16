namespace CoreVideoPro.MediaCore.Services;

public static class MediaCorePaths
{
    public const string ZoomSdkDirEnvVar = "ZOOM_SDK_DIR";
    public const string ZoomRuntimeDirEnvVar = "COREVIDEO_ZOOM_RUNTIME_DIR";
    public const string ZoomOAuthBrokerStartUrlEnvVar = "COREVIDEO_ZOOM_OAUTH_BROKER_START_URL";

    public static string StagedZoomRuntimeRelativePath =>
        Path.Combine("native-core", "zoom-runtime", "windows", "x64");

    public static string ArtifactsZoomRuntimeRelativePath =>
        Path.Combine("artifacts", "native", "zoom-runtime", "windows", "x64");

    public static string ZoomSdkStagingInstructions =>
        $"Set {ZoomSdkDirEnvVar} to the Zoom Meeting SDK x64 folder (contains h/zoom_sdk.h and bin/sdk.dll), " +
        $"then run .\\scripts\\stage-zoom-sdk.ps1. " +
        $"Alternatively set {ZoomRuntimeDirEnvVar} to an already-staged runtime folder.";

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

    public static IReadOnlyList<string> BuildZoomSdkArchitectureRootCandidates(string repoRoot)
    {
        var candidates = new List<string>();

        var runtimeDir = Environment.GetEnvironmentVariable(ZoomRuntimeDirEnvVar);
        if (!string.IsNullOrWhiteSpace(runtimeDir))
        {
            candidates.Add(runtimeDir);
        }

        candidates.Add(Path.Combine(repoRoot, StagedZoomRuntimeRelativePath));
        candidates.Add(Path.Combine(repoRoot, ArtifactsZoomRuntimeRelativePath));

        var sdkDir = Environment.GetEnvironmentVariable(ZoomSdkDirEnvVar);
        if (!string.IsNullOrWhiteSpace(sdkDir))
        {
            candidates.Add(sdkDir);
        }

        candidates.AddRange(
        [
            Path.Combine(repoRoot, "native", "build-dev"),
            Path.Combine(repoRoot, "native", "build"),
            Path.Combine(AppContext.BaseDirectory, "zoom-runtime", "windows", "x64"),
            AppContext.BaseDirectory
        ]);

        return candidates;
    }

    public static bool IsZoomSdkArchitectureRoot(string? candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate) || !Directory.Exists(candidate))
        {
            return false;
        }

        if (File.Exists(Path.Combine(candidate, "bin", "sdk.dll")))
        {
            return true;
        }

        return File.Exists(Path.Combine(candidate, "sdk.dll"));
    }

    public static string? ResolveZoomSdkArchitectureRoot(string? repoRoot = null)
    {
        repoRoot ??= RepoRoot;
        foreach (var candidate in BuildZoomSdkArchitectureRootCandidates(repoRoot))
        {
            if (!IsZoomSdkArchitectureRoot(candidate))
            {
                continue;
            }

            return Path.GetFullPath(candidate);
        }

        return null;
    }

    public static string ResolveStagedZoomRuntimeTarget(string? repoRoot = null)
    {
        repoRoot ??= RepoRoot;
        var overrideDir = Environment.GetEnvironmentVariable(ZoomRuntimeDirEnvVar);
        if (!string.IsNullOrWhiteSpace(overrideDir))
        {
            return Path.GetFullPath(overrideDir);
        }

        return Path.GetFullPath(Path.Combine(repoRoot, StagedZoomRuntimeRelativePath));
    }

    public static ZoomOAuthManifest LoadZoomOAuthManifest() => ZoomOAuthManifest.Load();

    public static bool IsZoomOAuthBrokerConfigured() => LoadZoomOAuthManifest().BrokerConfigured;

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