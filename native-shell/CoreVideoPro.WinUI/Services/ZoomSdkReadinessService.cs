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
            PackagingPath = overrides.PackagingPath ?? input.PackagingPath
        };
    }

    public static ZoomSdkReadinessInput DeriveInputForEngine(bool engineRunning)
    {
        var baseInput = CreateEmbeddedInput();
        if (!engineRunning)
        {
            return baseInput;
        }

        var nativeExe = MediaCorePaths.ResolveNativeCoreExecutable();
        var runtimePresent = nativeExe is not null;
        return new ZoomSdkReadinessInput
        {
            Platform = ZoomSdkRuntimePlatform.Windows,
            SdkRuntimePresent = runtimePresent,
            SdkVersion = runtimePresent ? "zoom-engine" : null,
            AppKeyPresent = baseInput.AppKeyPresent,
            OauthConfigured = baseInput.AppKeyPresent,
            JwtBrokerConfigured = false,
            RawVideoEnabled = runtimePresent,
            RawAudioEnabled = runtimePresent,
            RawShareEnabled = runtimePresent,
            PackagingPath = nativeExe
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
                input.SdkRuntimePresent
                    ? $"Runtime found{(string.IsNullOrWhiteSpace(input.PackagingPath) ? "." : $" at {input.PackagingPath}.")}"
                    : "Zoom Meeting SDK runtime is missing from the packaged helper process."),
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
                input.JwtBrokerConfigured ? "SDK JWT broker is configured." : "SDK JWT broker is not configured."),
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

        var warnings = string.IsNullOrWhiteSpace(input.SdkVersion)
            ? new List<string> { "SDK version is unknown; include it in support bundles before beta." }
            : [];

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