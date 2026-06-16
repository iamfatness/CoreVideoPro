using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

public static class ZoomSdkReadinessService
{
    private const string DefaultAppKey = ZoomOAuthManifest.DefaultPublicClientId;

    public static bool AppKeyPresent() => ResolveAppKey().Length > 0;

    public static ZoomSdkReadinessInput CreateEmbeddedInput(ZoomSdkReadinessInput? overrides = null)
    {
        var input = new ZoomSdkReadinessInput
        {
            Platform = ZoomSdkRuntimePlatform.Windows,
            SdkRuntimePresent = false,
            AppKeyPresent = AppKeyPresent(),
            OauthConfigured = false,
            OAuthSignedIn = false,
            OAuthBrokerConfigured = false,
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
            OAuthSignedIn = overrides.OAuthSignedIn,
            OAuthBrokerConfigured = overrides.OAuthBrokerConfigured,
            RawVideoEnabled = overrides.RawVideoEnabled,
            RawAudioEnabled = overrides.RawAudioEnabled,
            RawShareEnabled = overrides.RawShareEnabled,
            PackagingPath = overrides.PackagingPath ?? input.PackagingPath,
            NativeCorePresent = overrides.NativeCorePresent,
            StagedRuntimeReady = overrides.StagedRuntimeReady,
            StagingTargetPath = overrides.StagingTargetPath ?? input.StagingTargetPath
        };
    }

    public static ZoomSdkReadinessInput DeriveInputForEngine(bool _, bool oauthSignedIn = false)
    {
        var baseInput = CreateEmbeddedInput();
        var oauthManifest = MediaCorePaths.LoadZoomOAuthManifest();
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
        var oauthBrokerConfigured = oauthManifest.BrokerConfigured;

        return new ZoomSdkReadinessInput
        {
            Platform = ZoomSdkRuntimePlatform.Windows,
            SdkRuntimePresent = sdkRuntimeReady,
            SdkVersion = sdkVersion,
            AppKeyPresent = baseInput.AppKeyPresent,
            OauthConfigured = oauthBrokerConfigured || oauthSignedIn || baseInput.AppKeyPresent,
            OAuthSignedIn = oauthSignedIn,
            OAuthBrokerConfigured = oauthBrokerConfigured,
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
                "oauth-broker",
                input.OAuthBrokerConfigured,
                "OAuth PKCE broker",
                input.OAuthBrokerConfigured
                    ? $"Public Client OAuth + PKCE broker embedded ({ZoomOAuthManifest.DefaultBrokerStartUrl}, Zoom redirect {ZoomOAuthManifest.DefaultBrokerCallbackUrl})."
                    : $"OAuth PKCE broker is not configured. Embed src/config/zoomOAuth.json or set {MediaCorePaths.ZoomOAuthBrokerStartUrlEnvVar}."),
            BuildCheck(
                "oauth-session",
                true,
                "Zoom account sign-in",
                input.OAuthSignedIn
                    ? "Signed in with Zoom — broker mints Meeting SDK JWT via PKCE (same flow as CoreVideo plugin)."
                    : input.OAuthBrokerConfigured
                        ? "Not signed in. Use Sign in with Zoom for external-account meetings; public app key joins still work for dev."
                        : "OAuth sign-in unavailable until the PKCE broker is configured."),
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

        if (input.OAuthBrokerConfigured && !input.OAuthSignedIn)
        {
            warnings.Add("Sign in with Zoom (PKCE) before joining external-account meetings.");
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
        var oauthManifest = MediaCorePaths.LoadZoomOAuthManifest();
        if (oauthManifest.PublicClientIdPresent)
        {
            return oauthManifest.PublicClientId;
        }

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