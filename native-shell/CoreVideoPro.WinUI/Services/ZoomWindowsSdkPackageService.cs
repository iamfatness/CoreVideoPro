namespace CoreVideoPro.WinUI.Services;

public enum ZoomWindowsSdkArchitecture
{
    X64,
    Arm64
}

public class ZoomWindowsSdkRequiredFile
{
    public required string Id { get; init; }
    public required string RelativePath { get; init; }
    public required string Purpose { get; init; }
}

public sealed class ZoomWindowsSdkRequiredFileStatus
{
    public required string Id { get; init; }
    public required string RelativePath { get; init; }
    public required string Purpose { get; init; }
    public bool Present { get; init; }
    public long? SizeBytes { get; init; }
}

public sealed class ZoomWindowsSdkPackageReport
{
    public required string Status { get; init; }
    public ZoomWindowsSdkArchitecture Architecture { get; init; }
    public string? RootFolder { get; init; }
    public string? Version { get; init; }
    public required IReadOnlyList<ZoomWindowsSdkRequiredFileStatus> RequiredFiles { get; init; }
    public int RuntimeDllCount { get; init; }
    public int RuntimeExeCount { get; init; }
    public int RawMediaHeaderCount { get; init; }
    public required IReadOnlyList<string> Blockers { get; init; }
    public required IReadOnlyList<string> Warnings { get; init; }
    public required string Summary { get; init; }
    public bool IsReady => Status == "ready";
}

/// <summary>
/// Ports <c>zoomWindowsSdkPackage.ts</c> for WinUI SDK readiness diagnostics.
/// </summary>
public static class ZoomWindowsSdkPackageService
{
    public static readonly IReadOnlyList<ZoomWindowsSdkRequiredFile> RequiredFiles =
    [
        new()
        {
            Id = "sdk-dll",
            RelativePath = "bin/sdk.dll",
            Purpose = "Runtime DLL loaded by the helper process."
        },
        new()
        {
            Id = "sdk-lib",
            RelativePath = "lib/sdk.lib",
            Purpose = "Import library used by the Windows helper build."
        },
        new()
        {
            Id = "zoom-sdk-header",
            RelativePath = "h/zoom_sdk.h",
            Purpose = "SDK initialization header."
        },
        new()
        {
            Id = "meeting-service-header",
            RelativePath = "h/meeting_service_interface.h",
            Purpose = "Meeting join/status service header."
        },
        new()
        {
            Id = "raw-video-api-header",
            RelativePath = "h/rawdata/zoom_rawdata_api.h",
            Purpose = "Raw media API entry points."
        },
        new()
        {
            Id = "raw-video-renderer-header",
            RelativePath = "h/rawdata/rawdata_renderer_interface.h",
            Purpose = "Raw participant video and share renderer callbacks."
        },
        new()
        {
            Id = "raw-audio-header",
            RelativePath = "h/rawdata/rawdata_audio_helper_interface.h",
            Purpose = "Raw mixed and per-participant audio callbacks."
        }
    ];

    public static ZoomWindowsSdkPackageReport InspectDirectory(
        string architectureRoot,
        ZoomWindowsSdkArchitecture architecture = ZoomWindowsSdkArchitecture.X64,
        string? expectedVersion = null)
    {
        var normalizedRoot = Path.GetFullPath(architectureRoot);
        var rootFolder = Path.GetFileName(normalizedRoot.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (rootFolder.StartsWith("x64", StringComparison.OrdinalIgnoreCase) ||
            rootFolder.StartsWith("arm64", StringComparison.OrdinalIgnoreCase))
        {
            rootFolder = Path.GetFileName(Path.GetDirectoryName(normalizedRoot) ?? normalizedRoot);
        }

        var version = VersionFromRoot(rootFolder);
        var requiredFiles = RequiredFiles
            .Select(file =>
            {
                var fullPath = Path.Combine(normalizedRoot, file.RelativePath.Replace('/', Path.DirectorySeparatorChar));
                var present = File.Exists(fullPath);
                long? sizeBytes = present ? new FileInfo(fullPath).Length : null;
                return new ZoomWindowsSdkRequiredFileStatus
                {
                    Id = file.Id,
                    RelativePath = file.RelativePath,
                    Purpose = file.Purpose,
                    Present = present,
                    SizeBytes = sizeBytes
                };
            })
            .ToList();

        var binDirectory = Path.Combine(normalizedRoot, "bin");
        var rawDataDirectory = Path.Combine(normalizedRoot, "h", "rawdata");
        var runtimeDllCount = Directory.Exists(binDirectory)
            ? Directory.EnumerateFiles(binDirectory, "*.dll", SearchOption.TopDirectoryOnly).Count()
            : 0;
        var runtimeExeCount = Directory.Exists(binDirectory)
            ? Directory.EnumerateFiles(binDirectory, "*.exe", SearchOption.TopDirectoryOnly).Count()
            : 0;
        var rawMediaHeaderCount = Directory.Exists(rawDataDirectory)
            ? Directory.EnumerateFiles(rawDataDirectory, "*.h", SearchOption.TopDirectoryOnly).Count()
            : 0;

        var architectureLabel = architecture == ZoomWindowsSdkArchitecture.Arm64 ? "arm64" : "x64";
        var blockers = new List<string>();
        if (!Directory.Exists(normalizedRoot))
        {
            blockers.Add("SDK architecture root folder was not found.");
        }

        if (!string.IsNullOrWhiteSpace(expectedVersion) &&
            !string.IsNullOrWhiteSpace(version) &&
            !version.Equals(expectedVersion, StringComparison.Ordinal))
        {
            blockers.Add($"SDK version {version} does not match expected {expectedVersion}.");
        }

        blockers.AddRange(requiredFiles
            .Where(file => !file.Present)
            .Select(file => $"{architectureLabel}/{file.RelativePath} is missing."));

        var warnings = new List<string>();
        if (runtimeDllCount < 20)
        {
            warnings.Add($"Only {runtimeDllCount} runtime DLLs found; packaged helper may be missing Zoom dependencies.");
        }

        if (runtimeExeCount == 0)
        {
            warnings.Add("No runtime EXE helpers found; zTscoder/crash/helper tools may be missing.");
        }

        if (rawMediaHeaderCount < 3)
        {
            warnings.Add("Raw media headers are incomplete.");
        }

        if (string.IsNullOrWhiteSpace(version))
        {
            warnings.Add("Could not infer SDK version from SDK root folder.");
        }

        var status = blockers.Count == 0 ? "ready" : "blocked";
        var summary = status == "ready"
            ? $"Zoom Windows Meeting SDK {version ?? "unknown"} {architectureLabel} package is ready for helper packaging."
            : $"Zoom Windows Meeting SDK package blocked by {blockers.Count} issue{(blockers.Count == 1 ? "" : "s")}.";

        return new ZoomWindowsSdkPackageReport
        {
            Status = status,
            Architecture = architecture,
            RootFolder = rootFolder,
            Version = version,
            RequiredFiles = requiredFiles,
            RuntimeDllCount = runtimeDllCount,
            RuntimeExeCount = runtimeExeCount,
            RawMediaHeaderCount = rawMediaHeaderCount,
            Blockers = blockers,
            Warnings = warnings,
            Summary = summary
        };
    }

    public static string? VersionFromRoot(string? rootFolder)
    {
        if (string.IsNullOrWhiteSpace(rootFolder))
        {
            return null;
        }

        const string prefix = "zoom-sdk-windows-";
        return rootFolder.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)
            ? rootFolder[prefix.Length..]
            : null;
    }
}