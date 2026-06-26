namespace CoreVideoPro.MediaCore.Services;

public static class MediaCorePaths
{
    public const string ZoomSdkDirEnvVar = "ZOOM_SDK_DIR";
    public const string ZoomRuntimeDirEnvVar = "COREVIDEO_ZOOM_RUNTIME_DIR";
    public const string ZoomOAuthBrokerStartUrlEnvVar = "COREVIDEO_ZOOM_OAUTH_BROKER_START_URL";
    public const string FfmpegBinDirEnvVar = "COREVIDEO_FFMPEG_BIN_DIR";

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
                    (Directory.Exists(Path.Combine(dir, "studio")) ||
                     Directory.Exists(Path.Combine(dir, "native-shell"))))
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

            // Packaged layout (artifacts/native/win-unpacked) has no repo markers.
            return AppContext.BaseDirectory;
        }
    }

    public static string? ResolveNativeCoreExecutable()
    {
        var repo = RepoRoot;
        var candidates = BuildNativeCoreExecutableCandidates(repo, AppContext.BaseDirectory);

        return candidates.FirstOrDefault(File.Exists);
    }

    public static IReadOnlyList<string> BuildNativeCoreExecutableCandidates(string repoRoot, string appBaseDirectory) =>
    [
        Path.Combine(repoRoot, "native", "build-dev", "corevideo-native.exe"),
        Path.Combine(repoRoot, "native", "build-dev", "Release", "corevideo-native.exe"),
        Path.Combine(appBaseDirectory, "corevideo-native.exe"),
        Path.Combine(repoRoot, "native", "build", "corevideo-native.exe"),
        Path.Combine(repoRoot, "native", "build", "Release", "corevideo-native.exe")
    ];

    public static string? ResolveZoomEngineExecutable()
    {
        var repo = RepoRoot;
        var candidates = BuildZoomEngineExecutableCandidates(repo, AppContext.BaseDirectory);

        return candidates.FirstOrDefault(File.Exists);
    }

    public static IReadOnlyList<string> BuildZoomEngineExecutableCandidates(string repoRoot, string appBaseDirectory) =>
    [
        Path.Combine(repoRoot, "native", "build-dev", "corevideo-zoom-engine.exe"),
        Path.Combine(repoRoot, "native", "build-dev", "Release", "corevideo-zoom-engine.exe"),
        Path.Combine(appBaseDirectory, "corevideo-zoom-engine.exe"),
        Path.Combine(repoRoot, "native", "build", "corevideo-zoom-engine.exe"),
        Path.Combine(repoRoot, "native", "build", "Release", "corevideo-zoom-engine.exe")
    ];

    public static string ResolveMediaCoreWorkingDirectory()
    {
        var nativeExe = ResolveNativeCoreExecutable();
        if (!string.IsNullOrWhiteSpace(nativeExe))
        {
            return Path.GetDirectoryName(nativeExe) ?? AppContext.BaseDirectory;
        }

        return AppContext.BaseDirectory;
    }

    public static string? ResolvePackagedZoomRuntimeDirectory()
    {
        var appDir = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(appDir, "zoom-runtime", "windows", "x64"),
            Path.Combine(ResolveMediaCoreWorkingDirectory(), "zoom-runtime", "windows", "x64"),
            ResolveZoomSdkArchitectureRoot()
        };

        foreach (var candidate in candidates)
        {
            if (string.IsNullOrWhiteSpace(candidate))
            {
                continue;
            }

            if (File.Exists(Path.Combine(candidate, "bin", "sdk.dll")))
            {
                return candidate;
            }
        }

        return null;
    }

    public static IReadOnlyDictionary<string, string> BuildMediaCoreChildEnvironment()
    {
        var env = new Dictionary<string, string>(StringComparer.Ordinal);
        var workingDirectory = ResolveMediaCoreWorkingDirectory();

        var zoomEngine = ResolveZoomEngineExecutable();
        if (!string.IsNullOrWhiteSpace(zoomEngine))
        {
            env["COREVIDEO_ZOOM_ENGINE_PATH"] = zoomEngine;
        }

        var zoomRuntime = ResolvePackagedZoomRuntimeDirectory();
        if (!string.IsNullOrWhiteSpace(zoomRuntime))
        {
            env[ZoomRuntimeDirEnvVar] = zoomRuntime;
        }

        env["COREVIDEO_ZOOM_JOIN_WAIT_MS"] = "45000";

        var pathEntries = new List<string> { workingDirectory };
        var ffmpegBinDirectory = Environment.GetEnvironmentVariable(FfmpegBinDirEnvVar) ??
                                 Environment.GetEnvironmentVariable("FFMPEG_BIN_DIR");
        if (!string.IsNullOrWhiteSpace(ffmpegBinDirectory) && Directory.Exists(ffmpegBinDirectory))
        {
            env[FfmpegBinDirEnvVar] = ffmpegBinDirectory;
            pathEntries.Add(ffmpegBinDirectory);
        }

        var packagedBin = zoomRuntime is not null
            ? Path.Combine(zoomRuntime, "bin")
            : null;
        if (!string.IsNullOrWhiteSpace(packagedBin) && Directory.Exists(packagedBin))
        {
            pathEntries.Add(packagedBin);
        }

        var existingPath = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        pathEntries.Add(existingPath);
        env["PATH"] = string.Join(';', pathEntries.Where(static entry => !string.IsNullOrWhiteSpace(entry)));

        return env;
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
            Path.Combine(repoRoot, "ZoomSDK", "zoom-sdk-windows-7.0.5.39292", "x64"),
            Path.Combine(repoRoot, "ZoomSDK", "zoom-sdk-windows-7.0.5.39292"),
            Path.Combine(repoRoot, "ZoomSDK", "x64"),
            Path.Combine(repoRoot, "ZoomSDK"),
            Path.Combine(AppContext.BaseDirectory, "zoom-runtime", "windows", "x64")
        ]);

        return candidates;
    }

    public static bool IsZoomSdkArchitectureRoot(string? candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate) || !Directory.Exists(candidate))
        {
            return false;
        }

        return File.Exists(Path.Combine(candidate, "bin", "sdk.dll")) &&
               File.Exists(Path.Combine(candidate, "h", "zoom_sdk.h"));
    }

    public static string? NormalizeZoomSdkArchitectureRoot(string? candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate) || !Directory.Exists(candidate))
        {
            return null;
        }

        var normalized = Path.GetFullPath(candidate);
        if (IsZoomSdkArchitectureRoot(normalized))
        {
            return normalized;
        }

        var architectureRoot = Path.Combine(normalized, "x64");
        return IsZoomSdkArchitectureRoot(architectureRoot) ? architectureRoot : null;
    }

    public static string? ResolveZoomSdkArchitectureRoot(string? repoRoot = null)
    {
        repoRoot ??= RepoRoot;
        foreach (var candidate in BuildZoomSdkArchitectureRootCandidates(repoRoot))
        {
            var resolved = NormalizeZoomSdkArchitectureRoot(candidate);
            if (resolved is null)
            {
                continue;
            }

            return resolved;
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
}
