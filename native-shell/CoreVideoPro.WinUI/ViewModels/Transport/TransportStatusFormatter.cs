using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>
/// Pure, side-effect-free transport status / rollback / validation helpers extracted
/// verbatim from <see cref="StudioViewModel"/> as PR1 of the StudioViewModel strangler
/// refactor. Move-only: every signature and body matches the original. StudioViewModel
/// keeps thin private forwarders so its internal call sites are unchanged; the WinUI.Tests
/// characterization suite now targets this class directly (that green run is the parity
/// proof).
/// </summary>
public static class TransportStatusFormatter
{
    public static bool ResolveStreamingStateAfterFailedRetry(bool requestedStarting) => !requestedStarting;

    public static string FormatStreamSyncRetryExhaustedStatus(bool requestedStarting) =>
        requestedStarting
            ? "Streaming start failed: Media core is busy applying changes. Wait a moment and try Stream again."
            : "Streaming stop failed: Media core is busy applying changes. Wait a moment and try Stream again.";

    public static bool IsStreamingStartProven(
        NativeMediaCoreStateSnapshot snapshot,
        IReadOnlyList<string> requestedDestinations)
    {
        if (requestedDestinations.Count == 0)
        {
            return false;
        }

        return requestedDestinations.Any(destination =>
            snapshot.OutputSenderSession.Senders.Any(sender =>
                sender.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase) &&
                sender.Status is "starting" or "live") ||
            snapshot.OutputHealth.Any(item =>
                item.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase) &&
                item.Status == "live"));
    }

    public static bool TryFormatStreamingStartNoSenderFailure(
        NativeMediaCoreStateSnapshot snapshot,
        IReadOnlyList<string> requestedDestinations,
        out string failureStatus)
    {
        failureStatus = string.Empty;
        if (requestedDestinations.Count == 0)
        {
            return false;
        }

        if (IsStreamingStartProven(snapshot, requestedDestinations))
        {
            return false;
        }

        var unavailableSender = snapshot.OutputSenderSession.Senders.FirstOrDefault(sender =>
            requestedDestinations.Any(destination => sender.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase)) &&
            IsUnavailableOutputSenderWarning(sender));
        if (unavailableSender is not null)
        {
            var unavailableDetail = BuildOutputSenderFailureDetail(unavailableSender);
            failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(unavailableDetail));
            return true;
        }

        var destinations = string.Join(", ", requestedDestinations.Select(destination => destination.ToUpperInvariant()));
        var senderState = $"{snapshot.OutputSenderSession.Status}:{snapshot.OutputSenderSession.ActiveSenderCount}";
        var detail = $"Selected stream destinations ({destinations}) did not arm a native output sender. Sender state {senderState}.";
        failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(detail));
        return true;
    }

    private static bool IsUnavailableOutputSenderWarning(NativeMediaCoreOutputSender sender)
    {
        var detail = string.Join(
            " ",
            new[] { sender.LastResultCode, sender.Warning, sender.RuntimeDetail, sender.LastError }
                .Where(static part => !string.IsNullOrWhiteSpace(part)))
            .ToLowerInvariant();
        return detail.Contains("output-unavailable", StringComparison.Ordinal) ||
               detail.Contains("not available in this build", StringComparison.Ordinal) ||
               detail.Contains("runtime-missing", StringComparison.Ordinal) ||
               detail.Contains("libndi runtime", StringComparison.Ordinal) ||
               detail.Contains("no ndi sender module", StringComparison.Ordinal) ||
               detail.Contains("no srt sender module", StringComparison.Ordinal);
    }

    private static bool IsUnavailableOutputHealthWarning(NativeMediaCoreOutputHealth item)
    {
        var detail = item.Message?.ToLowerInvariant() ?? string.Empty;
        return detail.Contains("output-unavailable", StringComparison.Ordinal) ||
               detail.Contains("not available in this build", StringComparison.Ordinal) ||
               detail.Contains("runtime-missing", StringComparison.Ordinal) ||
               detail.Contains("libndi runtime", StringComparison.Ordinal) ||
               detail.Contains("no ndi sender module", StringComparison.Ordinal) ||
               detail.Contains("no srt sender module", StringComparison.Ordinal);
    }

    public static bool ResolveRecordingStateAfterFailedRetry(bool requestedStarting) => !requestedStarting;

    public static string FormatRecordingSyncRetryExhaustedStatus(bool requestedStarting) =>
        requestedStarting
            ? "Recording start failed: Media core is busy applying changes. Wait a moment and try Record again."
            : "Recording stop failed: Media core is busy applying changes. Wait a moment and try Record again.";

    public static bool TryFormatRecordingStartHealthFailure(NativeMediaCoreStateSnapshot snapshot, out string failureStatus)
    {
        var detail = snapshot.OutputHealth
            .Where(item =>
                item.Status is "failed" or "warning" &&
                item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase))
            .Select(item => item.Message)
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        detail ??= snapshot.Recording is { Active: true, Status: "failed" or "warning" } recording
            ? recording.Error ?? recording.Warning
            : null;

        if (string.IsNullOrWhiteSpace(detail))
        {
            failureStatus = string.Empty;
            return false;
        }

        failureStatus = FormatRecordingFailureStatus("start", new InvalidOperationException(detail));
        return true;
    }

    public static bool TryFormatStreamingStartHealthFailure(NativeMediaCoreStateSnapshot snapshot, out string failureStatus)
    {
        var detail = snapshot.OutputSenderSession.Senders
            .Where(static sender => sender.Status is "failed" or "warning")
            .Select(BuildOutputSenderFailureDetail)
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        detail ??= snapshot.OutputHealth
            .Where(item =>
                item.Status is "failed" or "warning" &&
                !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase))
            .Select(item => item.Message)
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        detail ??= snapshot.OutputSenderSession.Warnings
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        if (string.IsNullOrWhiteSpace(detail))
        {
            failureStatus = string.Empty;
            return false;
        }

        failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(detail));
        return true;
    }

    private static string BuildOutputSenderFailureDetail(NativeMediaCoreOutputSender sender) =>
        string.Join(
            " ",
            new[] { sender.LastResultCode, sender.Warning, sender.RuntimeDetail, sender.LastError }
                .Where(static part => !string.IsNullOrWhiteSpace(part)));

    public static string FormatRecordingFailureStatus(string action, Exception exception)
    {
        var detail = NormalizeStreamingFailureDetail(exception.Message);
        var lowered = detail.ToLowerInvariant();
        var mediaCoreFailure =
            lowered.Contains("media core", StringComparison.Ordinal) ||
            lowered.Contains("media-core", StringComparison.Ordinal) ||
            lowered.Contains("native media core", StringComparison.Ordinal) ||
            lowered.Contains("native-media-core", StringComparison.Ordinal) ||
            lowered.Contains("json-rpc", StringComparison.Ordinal) ||
            lowered.Contains("json rpc", StringComparison.Ordinal) ||
            lowered.Contains("broken pipe", StringComparison.Ordinal) ||
            lowered.Contains("process exited", StringComparison.Ordinal) ||
            lowered.Contains("process is not running", StringComparison.Ordinal);
        var prefix = lowered.Contains("still applying another output change", StringComparison.Ordinal) ||
                     lowered.Contains("try again", StringComparison.Ordinal) && lowered.Contains("media core", StringComparison.Ordinal)
            ? "Media core is busy applying changes. Wait a moment and try Record again."
            : lowered.Contains("program frame", StringComparison.Ordinal) ||
              lowered.Contains("program pixels", StringComparison.Ordinal)
                ? "Program video is not ready. Put a valid source on Program before recording."
            : lowered.Contains("target folder", StringComparison.Ordinal) ||
              lowered.Contains("path", StringComparison.Ordinal) ||
              lowered.Contains("directory", StringComparison.Ordinal) ||
              lowered.Contains("disk", StringComparison.Ordinal)
                ? "Recording target is not ready. Check the folder path and disk space."
            : mediaCoreFailure
                ? "Media core failed while starting recording. Open Details for the native error."
                : "Media core rejected the recording request.";

        var normalizedAction = string.IsNullOrWhiteSpace(action) ? "request" : action.Trim();
        return $"Recording {normalizedAction} failed: {prefix} {detail}";
    }

    public static string FormatStreamingFailureStatus(string action, Exception exception)
    {
        var rawLowered = exception.Message?.ToLowerInvariant() ?? string.Empty;
        var detail = NormalizeStreamingFailureDetail(exception.Message);
        var lowered = detail.ToLowerInvariant();
        var rtmpContext =
            rawLowered.Contains("rtmp", StringComparison.Ordinal) ||
            rawLowered.Contains("rtmps", StringComparison.Ordinal) ||
            lowered.Contains("rtmp", StringComparison.Ordinal) ||
            lowered.Contains("rtmps", StringComparison.Ordinal);
        var ffmpegRuntimeMissing =
            lowered.Contains("ffmpeg executable", StringComparison.Ordinal) ||
            lowered.Contains("ffmpeg.exe was not found", StringComparison.Ordinal) ||
            lowered.Contains("ffmpeg folder not found", StringComparison.Ordinal) ||
            lowered.Contains("does not contain ffmpeg.exe", StringComparison.Ordinal) ||
            lowered.Contains("choose the bin folder", StringComparison.Ordinal);
        var mediaCoreFailure =
            lowered.Contains("media core", StringComparison.Ordinal) ||
            lowered.Contains("media-core", StringComparison.Ordinal) ||
            lowered.Contains("native media core", StringComparison.Ordinal) ||
            lowered.Contains("native-media-core", StringComparison.Ordinal) ||
            lowered.Contains("json-rpc", StringComparison.Ordinal) ||
            lowered.Contains("json rpc", StringComparison.Ordinal) ||
            lowered.Contains("broken pipe", StringComparison.Ordinal) ||
            lowered.Contains("process exited", StringComparison.Ordinal) ||
            lowered.Contains("process is not running", StringComparison.Ordinal);
        var prefix = lowered.Contains("still applying another output change", StringComparison.Ordinal) ||
                     lowered.Contains("try again", StringComparison.Ordinal) && lowered.Contains("media core", StringComparison.Ordinal)
            ? "Media core is busy applying changes. Wait a moment and try Stream again."
            : lowered.Contains("program frame", StringComparison.Ordinal) ||
                     lowered.Contains("program pixels", StringComparison.Ordinal)
            ? "Program video is not ready. Put a valid source on Program before streaming."
            : lowered.Contains("did not arm a native output sender", StringComparison.Ordinal) ||
              lowered.Contains("sender state idle", StringComparison.Ordinal)
                ? "Native output sender did not start. Check Stream settings and open Health for sender diagnostics."
            : lowered.Contains("select at least one stream destination", StringComparison.Ordinal)
                ? "No stream destination is selected. Enable RTMP, NDI, or SRT before streaming."
            : lowered.Contains("configure rtmp", StringComparison.Ordinal) ||
              lowered.Contains("rtmp server url", StringComparison.Ordinal) ||
              lowered.Contains("rtmp stream key", StringComparison.Ordinal)
                ? "RTMP settings are incomplete. Configure the server URL and stream key before streaming."
            : lowered.Contains("configure an ndi program name", StringComparison.Ordinal)
                ? "NDI settings are incomplete. Set the NDI program name before streaming."
            : lowered.Contains("ndi", StringComparison.Ordinal) &&
              (lowered.Contains("output-unavailable", StringComparison.Ordinal) ||
               lowered.Contains("no ndi sender module", StringComparison.Ordinal) ||
               lowered.Contains("not available in this build", StringComparison.Ordinal) ||
               lowered.Contains("libndi runtime", StringComparison.Ordinal) ||
               lowered.Contains("runtime-missing", StringComparison.Ordinal) ||
               lowered.Contains("missing ndi-output", StringComparison.Ordinal))
                ? "NDI output is not available. Install the NDI runtime or use a build with NDI output enabled."
            : lowered.Contains("srt", StringComparison.Ordinal) &&
              (lowered.Contains("configure", StringComparison.Ordinal) ||
               lowered.Contains("must", StringComparison.Ordinal) ||
               lowered.Contains("passphrase", StringComparison.Ordinal))
                ? "SRT settings are incomplete. Check host, port, latency, and passphrase before streaming."
            : lowered.Contains("srt", StringComparison.Ordinal) &&
              (lowered.Contains("output-unavailable", StringComparison.Ordinal) ||
               lowered.Contains("no srt sender module", StringComparison.Ordinal) ||
               lowered.Contains("not available in this build", StringComparison.Ordinal) ||
               lowered.Contains("missing srt-output", StringComparison.Ordinal))
                ? "SRT output is not available in this build. Use RTMP/NDI or install a build with SRT output enabled."
            : lowered.Contains("ffmpeg", StringComparison.Ordinal) && ffmpegRuntimeMissing
                ? "FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > FFmpeg."
                : rtmpContext ||
                  lowered.Contains("connection refused", StringComparison.Ordinal)
                    ? "RTMP output failed. Check the server URL, stream key, and network."
                    : mediaCoreFailure
                        ? "Media core failed while starting stream. Open Details for the native error."
                        : "Media core rejected the stream request.";

        var normalizedAction = string.IsNullOrWhiteSpace(action) ? "request" : action.Trim();
        return $"Streaming {normalizedAction} failed: {prefix} {detail}";
    }

    public static string FormatOutputStatusBrief(string? status)
    {
        if (string.IsNullOrWhiteSpace(status))
        {
            return "Outputs idle";
        }

        var normalized = status.Trim();
        if (normalized.StartsWith("Streaming start failed:", StringComparison.OrdinalIgnoreCase))
        {
            return FormatStreamingFailureBrief(normalized, "Streaming start failed");
        }

        if (normalized.StartsWith("Streaming stop failed:", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream stop failed";
        }

        if (normalized.StartsWith("Recording start failed:", StringComparison.OrdinalIgnoreCase))
        {
            return FormatRecordingFailureBrief(normalized, "Recording start failed");
        }

        if (normalized.StartsWith("Recording stop failed:", StringComparison.OrdinalIgnoreCase))
        {
            return "Recording stop failed";
        }

        if (normalized.StartsWith("Streaming settings sync failed:", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream settings failed";
        }

        if (normalized.StartsWith("Streaming settings failed:", StringComparison.OrdinalIgnoreCase))
        {
            return FormatStreamingFailureBrief(normalized, "Stream settings failed");
        }

        if (normalized.StartsWith("RTMP output failed:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("RTMPS output failed:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("SRT output failed:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("NDI output failed:", StringComparison.OrdinalIgnoreCase))
        {
            var separatorIndex = normalized.IndexOf(':', StringComparison.Ordinal);
            return separatorIndex > 0 ? normalized[..separatorIndex] : "Streaming failed";
        }

        if (normalized.StartsWith("Output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("Output failed:", StringComparison.OrdinalIgnoreCase))
        {
            if (normalized.Contains("rtmp", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("rtmps", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("ffmpeg", StringComparison.OrdinalIgnoreCase))
            {
                return normalized.Contains("failed", StringComparison.OrdinalIgnoreCase)
                    ? "RTMP output failed"
                    : "RTMP output warning";
            }

            return normalized.Contains("failed", StringComparison.OrdinalIgnoreCase)
                ? "Output failed"
                : "Output warning";
        }

        if (normalized.StartsWith("RTMP output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("RTMPS output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("SRT output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("NDI output warning:", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream warning";
        }

        if (normalized.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
            normalized.Contains("error", StringComparison.OrdinalIgnoreCase))
        {
            return "Output failed";
        }

        return normalized.Length <= 28 ? normalized : $"{normalized[..25]}...";
    }

    private static string FormatRecordingFailureBrief(string normalized, string fallback)
    {
        if (normalized.Contains("Program video is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "Program video not ready";
        }

        if (normalized.Contains("Recording target is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "Recording target not ready";
        }

        if (normalized.Contains("Media core rejected", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core rejected recording";
        }

        if (normalized.Contains("Media core failed", StringComparison.OrdinalIgnoreCase))
        {
            if (normalized.Contains("process exited", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("process is not running", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core exited";
            }

            if (normalized.Contains("broken pipe", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core pipe failed";
            }

            return "Media core failed";
        }

        if (normalized.Contains("Media core is busy", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core busy";
        }

        return fallback;
    }

    private static string FormatStreamingFailureBrief(string normalized, string fallback)
    {
        if (normalized.Contains("Program video is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "Program video not ready";
        }

        if (normalized.Contains("Native output sender did not start", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream sender not armed";
        }

        if (normalized.Contains("RTMP output failed", StringComparison.OrdinalIgnoreCase))
        {
            return "RTMP output failed";
        }

        if (normalized.Contains("No stream destination is selected", StringComparison.OrdinalIgnoreCase))
        {
            return "No stream destination";
        }

        if (normalized.Contains("RTMP settings are incomplete", StringComparison.OrdinalIgnoreCase))
        {
            return "RTMP settings missing";
        }

        if (normalized.Contains("NDI settings are incomplete", StringComparison.OrdinalIgnoreCase))
        {
            return "NDI settings missing";
        }

        if (normalized.Contains("NDI output is not available", StringComparison.OrdinalIgnoreCase))
        {
            return "NDI unavailable";
        }

        if (normalized.Contains("SRT settings are incomplete", StringComparison.OrdinalIgnoreCase))
        {
            return "SRT settings missing";
        }

        if (normalized.Contains("SRT output is not available", StringComparison.OrdinalIgnoreCase))
        {
            return "SRT unavailable";
        }

        if (normalized.Contains("FFmpeg is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "FFmpeg not ready";
        }

        if (normalized.Contains("Media core rejected", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core rejected stream";
        }

        if (normalized.Contains("Media core failed", StringComparison.OrdinalIgnoreCase))
        {
            if (normalized.Contains("process exited", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("process is not running", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core exited";
            }

            if (normalized.Contains("broken pipe", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core pipe failed";
            }

            return "Media core failed";
        }

        if (normalized.Contains("Media core is busy", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core busy";
        }

        return fallback;
    }

    public static bool ShouldShowOutputStatusDetails(string? status)
    {
        if (string.IsNullOrWhiteSpace(status))
        {
            return false;
        }

        var normalized = status.Trim();
        return normalized.Length > 28 ||
            normalized.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
            normalized.Contains("error", StringComparison.OrdinalIgnoreCase) ||
            normalized.Contains("rejected", StringComparison.OrdinalIgnoreCase);
    }

    internal static string NormalizeStreamingFailureDetail(string? message)
    {
        var detail = string.IsNullOrWhiteSpace(message)
            ? "No native error detail was returned."
            : message.Trim();

        if (detail.Contains("media-core sync in flight", StringComparison.OrdinalIgnoreCase) ||
            detail.Contains("sync in flight", StringComparison.OrdinalIgnoreCase) &&
            detail.Contains("backpressure", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core is still applying another output change. Wait a moment and try again.";
        }

        string[] noisyPrefixes =
        [
            "media-core sync failed:",
            "native-media-core-sync failed:",
            "native media core sync failed:",
            "media-core request failed:",
            "native media core request failed:",
            "start-program-output failed:",
            "start-program-output failed.",
            "program-output failed:",
            "program-output failed.",
            "stop-program-output failed:",
            "stop-program-output failed.",
            "output sender failed during sync:",
            "output sender failed:",
            "rtmp output sender failed:",
            "rtmp sender failed:"
        ];

        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var prefix in noisyPrefixes)
            {
                if (!detail.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                detail = detail[prefix.Length..].Trim();
                changed = true;
            }
        }

        if (detail.Equals("missing:ffmpeg executable", StringComparison.OrdinalIgnoreCase))
        {
            return "FFmpeg executable was not found in the configured bin folder, app folder, or PATH.";
        }

        return detail;
    }

    public static string? ValidateStreamDestinationCapabilities(
        bool streamRtmpEnabled,
        bool streamNdiEnabled,
        bool streamSrtEnabled,
        NativeMediaCoreProfile? profile)
    {
        if (profile is null)
        {
            return null;
        }

        if (streamRtmpEnabled && !HasNativeOutputCapability(profile, "rtmp-output"))
        {
            return "RTMP output is selected, but the native media core profile is missing rtmp-output.";
        }

        if (streamNdiEnabled && !HasNativeOutputCapability(profile, "ndi-output"))
        {
            return "NDI output is selected, but the native media core profile is missing ndi-output.";
        }

        if (streamSrtEnabled && !HasNativeOutputCapability(profile, "srt-output"))
        {
            return "SRT output is selected, but the native media core profile is missing srt-output.";
        }

        return null;
    }

    internal static bool HasNativeOutputCapability(NativeMediaCoreProfile profile, string capability) =>
        profile.Capabilities.Contains(capability, StringComparer.OrdinalIgnoreCase);

    public static string ResolveProgramResolutionLabel(NativeMediaCoreStateSnapshot snapshot)
    {
        var profile = snapshot.OutputProfile;
        if (profile.Width > 0 && profile.Height > 0)
        {
            return TransportFormatting.ShortResolutionLabel($"{profile.Width}x{profile.Height}", profile.Fps);
        }

        return TransportFormatting.ShortResolutionLabel(profile.Resolution, profile.Fps);
    }
}
