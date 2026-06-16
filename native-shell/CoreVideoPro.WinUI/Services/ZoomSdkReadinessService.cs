using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class ZoomSdkReadinessService
{
    private const string DefaultAppKey = "y6sIWSwiTZe1JygMx4C9EQ";

    public static bool AppKeyPresent() => ResolveAppKey().Length > 0;

    public static ZoomSdkReadinessInput CreateEmbeddedInput(ZoomSdkReadinessInput? overrides = null)
    {
        var input = new ZoomSdkReadinessInput
        {
            Platform = ZoomSdkRuntimePlatform.Windows,
            SdkRuntimePresent = false,
            AppKeyPresent = AppKeyPresent(),
            OauthConfigured = false,
            JwtBrokerConfigured = false,
            RawVideoEnabled = false,
            RawAudioEnabled = false,
            RawShareEnabled = false
        };

        if (overrides is null)
        {
            return input;
        }

        return new ZoomSdkReadinessInput
        {
            Platform = overrides.Platform,
            SdkRuntimePresent = overrides.SdkRuntimePresent,
            SdkVersion = overrides.SdkVersion ?? input.SdkVersion,
            AppKeyPresent = overrides.AppKeyPresent,
            OauthConfigured = overrides.OauthConfigured,
            JwtBrokerConfigured = overrides.JwtBrokerConfigured,
            RawVideoEnabled = overrides.RawVideoEnabled,
            RawAudioEnabled = overrides.RawAudioEnabled,
            RawShareEnabled = overrides.RawShareEnabled,
            PackagingPath = overrides.PackagingPath ?? input.PackagingPath,
            NativeCorePresent = overrides.NativeCorePresent,
            StagedRuntimeReady = overrides.StagedRuntimeReady,
            StagingTargetPath = overrides.StagingTargetPath ?? input.StagingTargetPath
        };
    }

    public static ZoomSdkReadinessInput DeriveInputForEngine(bool _)
    {
        var baseInput = CreateEmbeddedInput();
        var nativeExe = MediaCorePaths.ResolveNativeCoreExecutable();
        var zoomSdkRoot = MediaCorePaths.ResolveZoomSdkArchitectureRoot();
        var stagedTarget = MediaCorePaths.ResolveStagedZoomRuntimeTarget();
        var packageReport = zoomSdkRoot is not null
            ? ZoomWindowsSdkPackageService.InspectDirectory(zoomSdkRoot)
            : null;
        var sdkDllBesideNative = nativeExe is not null &&
                                 File.Exists(Path.Combine(Path.GetDirectoryName(nativeExe) ?? string.Empty, "sdk.dll"));
        var stagedRuntimeReady = packageReport?.IsReady == true &&
                                 zoomSdkRoot is not null &&
                                 string.Equals(
                                     Path.GetFullPath(zoomSdkRoot),
                                     Path.GetFullPath(stagedTarget),
                                     StringComparison.OrdinalIgnoreCase);
        var sdkRuntimeReady = packageReport?.IsReady == true || sdkDllBesideNative;
        var sdkVersion = packageReport?.Version ?? (sdkRuntimeReady ? "zoom-engine" : null);
        var packagingPath = zoomSdkRoot ?? nativeExe;
        var jwtBrokerConfigured = MediaCorePaths.IsZoomJwtBrokerConfigured();

        return new ZoomSdkReadinessInput
        {
            Platform = ZoomSdkRuntimePlatform.Windows,
            SdkRuntimePresent = sdkRuntimeReady,
            SdkVersion = sdkVersion,
            AppKeyPresent = baseInput.AppKeyPresent,
            OauthConfigured = baseInput.AppKeyPresent,
            JwtBrokerConfigured = jwtBrokerConfigured,
            RawVideoEnabled = packageReport?.RequiredFiles.FirstOrDefault(file => file.Id == "raw-video-api-header")?.Present == true
                              || sdkRuntimeReady,
            RawAudioEnabled = packageReport?.RequiredFiles.FirstOrDefault(file => file.Id == "raw-audio-header")?.Present == true
                              || sdkRuntimeReady,
            RawShareEnabled = packageReport?.RequiredFiles.FirstOrDefault(file => file.Id == "raw-video-renderer-header")?.Present == true
                              || sdkRuntimeReady,
            PackagingPath = packagingPath,
            NativeCorePresent = nativeExe is not null,
            StagedRuntimeReady = stagedRuntimeReady,
            StagingTargetPath = stagedTarget
        };
    }

    public static ZoomSdkReadinessReport Assess(ZoomSdkReadinessInput input)
    {
        var checks = new List<ZoomSdkReadinessCheck>
        {
            BuildCheck(
                "sdk-runtime",
                input.SdkRuntimePresent,
                "Zoom Meeting SDK runtime",
                DescribeSdkRuntimeDetail(input)),
            BuildCheck(
                "app-key",
                input.AppKeyPresent,
                "Meeting SDK app key",
                input.AppKeyPresent ? "Meeting SDK app key is configured." : "Meeting SDK app key is missing."),
            BuildCheck(
                "oauth",
                input.OauthConfigured,
                "OAuth broker",
                input.OauthConfigured ? "OAuth broker is configured for meeting authorization." : "OAuth broker is not configured."),
            BuildCheck(
                "jwt-broker",
                input.JwtBrokerConfigured,
                "SDK JWT broker",
                input.JwtBrokerConfigured
                    ? $"SDK JWT broker is configured ({MediaCorePaths.ZoomJwtBrokerUrlEnvVar})."
                    : $"SDK JWT broker is not configured. Set {MediaCorePaths.ZoomJwtBrokerUrlEnvVar} for dev JWT auth bypass messaging."),
            BuildCheck(
                "raw-video",
                input.RawVideoEnabled,
                "Raw participant video",
                input.RawVideoEnabled ? "Raw participant video callbacks are enabled." : "Raw participant video callbacks are disabled."),
            BuildCheck(
                "raw-audio",
                input.RawAudioEnabled,
                "Raw mixed and isolated audio",
                input.RawAudioEnabled ? "Raw audio callbacks are enabled." : "Raw audio callbacks are disabled."),
            BuildCheck(
                "raw-share",
                input.RawShareEnabled,
                "Raw screen share",
                input.RawShareEnabled ? "Raw screen-share callbacks are enabled." : "Raw screen-share callbacks are disabled.")
        };

        var blockers = checks
            .Where(check => check.Status == ZoomSdkReadinessStatus.Blocked)
            .Select(check => check.Detail)
            .ToList();

        var warnings = new List<string>();
        if (!input.SdkRuntimePresent && input.NativeCorePresent)
        {
            warnings.Add($"Native media core is built but Zoom SDK runtime is not staged at {input.StagingTargetPath}.");
        }

        if (!input.StagedRuntimeReady && input.SdkRuntimePresent)
        {
            warnings.Add(
                $"Zoom SDK runtime was discovered outside the staged target ({input.StagingTargetPath}). Run .\\scripts\\stage-zoom-sdk.ps1 to normalize packaging.");
        }

        if (string.IsNullOrWhiteSpace(input.SdkVersion))
        {
            warnings.Add("SDK version is unknown; include it in support bundles before beta.");
        }

        if (input.SdkRuntimePresent && input.PackagingPath is not null)
        {
            var packageRoot = Directory.Exists(input.PackagingPath)
                ? input.PackagingPath
                : Path.GetDirectoryName(input.PackagingPath);
            if (packageRoot is not null && Directory.Exists(packageRoot))
            {
                var packageReport = ZoomWindowsSdkPackageService.InspectDirectory(packageRoot);
                warnings.AddRange(packageReport.Warnings.Where(warning => !warnings.Contains(warning)));
                if (!packageReport.IsReady)
                {
                    foreach (var blocker in packageReport.Blockers.Where(blocker => !blockers.Contains(blocker)))
                    {
                        blockers.Add(blocker);
                        checks.Add(BuildCheck(
                            $"sdk-package-{checks.Count}",
                            false,
                            "Zoom SDK package",
                            blocker));
                    }
                }
            }
        }

        var status = blockers.Count > 0
            ? ZoomSdkReadinessStatus.Blocked
            : warnings.Count > 0
                ? ZoomSdkReadinessStatus.Warning
                : ZoomSdkReadinessStatus.Ready;

        var platformLabel = input.Platform == ZoomSdkRuntimePlatform.MacOs ? "macos" : "windows";
        var summary = status switch
        {
            ZoomSdkReadinessStatus.Ready => $"Zoom SDK media path ready on {platformLabel}.",
            ZoomSdkReadinessStatus.Warning => $"Zoom SDK media path usable on {platformLabel} with {warnings.Count} warning.",
            _ => $"Zoom SDK media path blocked by {blockers.Count} missing requirement{(blockers.Count == 1 ? "" : "s")}."
        };

        return new ZoomSdkReadinessReport
        {
            Status = status,
            Platform = input.Platform,
            SdkVersion = input.SdkVersion ?? "unknown",
            Checks = checks,
            Blockers = blockers,
            Warnings = warnings,
            Summary = summary
        };
    }

    public static bool ShouldBlockZoomJoin(bool engineRunning, ZoomSdkReadinessReport readiness) =>
        engineRunning && readiness.Status == ZoomSdkReadinessStatus.Blocked;

    private static string DescribeSdkRuntimeDetail(ZoomSdkReadinessInput input)
    {
        if (input.SdkRuntimePresent)
        {
            return $"Runtime found{(string.IsNullOrWhiteSpace(input.PackagingPath) ? "." : $" at {input.PackagingPath}.")}";
        }

        if (!input.NativeCorePresent)
        {
            return "Native media core executable is missing. Build with .\\scripts\\build-native-dev.ps1 after staging the SDK.";
        }

        return $"Zoom Meeting SDK runtime is missing from the staged helper target ({input.StagingTargetPath}). {MediaCorePaths.ZoomSdkStagingInstructions}";
    }

    private static ZoomSdkReadinessCheck BuildCheck(string id, bool passed, string label, string detail) =>
        new()
        {
            Id = id,
            Status = passed ? ZoomSdkReadinessStatus.Ready : ZoomSdkReadinessStatus.Blocked,
            Label = label,
            Detail = detail
        };

    private static string ResolveAppKey()
    {
        try
        {
            var manifestPath = Path.Combine(MediaCorePaths.RepoRoot, "src", "config", "zoomMeetingSdk.json");
            if (!File.Exists(manifestPath))
            {
                return DefaultAppKey;
            }

            using var document = JsonDocument.Parse(File.ReadAllText(manifestPath));
            if (document.RootElement.TryGetProperty("publicAppKey", out var keyElement))
            {
                return keyElement.GetString()?.Trim() ?? string.Empty;
            }
        }
        catch
        {
            // Fall back to embedded default.
        }

        return DefaultAppKey;
    }
}