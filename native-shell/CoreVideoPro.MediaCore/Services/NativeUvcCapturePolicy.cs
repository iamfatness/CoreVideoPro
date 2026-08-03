using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Decides when the shell should hand a camera to the native core's Media
/// Foundation UVC adapter instead of opening it locally via WinRT MediaCapture
/// and bridging BGRA frames over shared memory.
///
/// The native path is DEFAULT-ON (opt out with COREVIDEO_NATIVE_UVC=0) and
/// self-falls-back: the shell only commits when the core's
/// connect-capture-device response reports the device connected; anything else
/// (stub core without the UVC adapter, unknown device id, camera open failure)
/// keeps the existing WinUI MediaCapture bridge as the capture path.
/// </summary>
public static class NativeUvcCapturePolicy
{
    public const string EnvironmentVariableName = "COREVIDEO_NATIVE_UVC";

    public static bool IsEnabled(string? environmentValue)
    {
        // DEFAULT-ON (opt out with COREVIDEO_NATIVE_UVC=0). Native MF capture in the core
        // is the RIGHT perf path — the managed WinUI MediaCapture bridge copies every
        // webcam frame through managed memory (SafeBuffer.WriteSpan) at ~180MB/s for 1080p
        // cameras (dotnet-trace: ~50% of wall-clock, churns the shell to multi-GB heaps,
        // starving the UI thread = the operator stutter). The two blockers are FIXED and
        // rig-verified (2026-07-10): the frame-key id mismatch (pink tiles — outer
        // WinUiCaptureDeviceAdapter dropped outputSourceId) and the WGC teardown crash.
        // Native capture ran owner-verified through full sessions 2026-07-11/12
        // (~265MB flat vs multi-GB bridge, 0 drops). Per-device fallback to the bridge
        // still applies automatically whenever the core cannot connect a camera.
        var normalized = environmentValue?.Trim();
        return normalized is null ||
            !(normalized.Equals("0", StringComparison.Ordinal) ||
              normalized.Equals("false", StringComparison.OrdinalIgnoreCase) ||
              normalized.Equals("off", StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// Native capture applies to camera-class devices only. SDI cards
    /// (blackmagic/aja) are served by their vendor adapters, and SRT ingest is
    /// a virtual transport — both keep their existing paths.
    /// </summary>
    public static bool ShouldPreferNativeCapture(string? environmentValue, string vendor)
    {
        if (!IsEnabled(environmentValue))
        {
            return false;
        }

        return vendor.Equals("uvc", StringComparison.OrdinalIgnoreCase) ||
            vendor.Equals("windows", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Finds the device the core reports as connected for this shell device.
    /// Matches the SHA-256 stable id (ordinal — both sides hash the symbolic
    /// link with the same convention) or the OS symbolic link
    /// (case-insensitive, symbolic-link casing differs between WinRT and Media
    /// Foundation enumeration). Returns null when the core did not connect it.
    /// </summary>
    public static NativeCaptureDeviceStatus? FindConnectedDevice(
        IReadOnlyList<NativeCaptureDeviceStatus> devices,
        string stableDeviceId,
        string? nativeDeviceId)
    {
        foreach (var device in devices)
        {
            if (!device.ConnectionState.Equals("connected", StringComparison.Ordinal))
            {
                continue;
            }

            if (Matches(device, stableDeviceId, nativeDeviceId))
            {
                return device;
            }
        }

        return null;
    }

    /// <summary>
    /// Finds the core's status for this shell device REGARDLESS of connection
    /// state — used by the first-frame confirmation loop to observe a device
    /// transition from "connected" (waiting) to "error" (the native no-first-
    /// frame watchdog fired) or to signalPresent=true (a real frame arrived).
    /// </summary>
    public static NativeCaptureDeviceStatus? FindDevice(
        IReadOnlyList<NativeCaptureDeviceStatus> devices,
        string stableDeviceId,
        string? nativeDeviceId)
    {
        foreach (var device in devices)
        {
            if (Matches(device, stableDeviceId, nativeDeviceId))
            {
                return device;
            }
        }

        return null;
    }

    private static bool Matches(NativeCaptureDeviceStatus device, string stableDeviceId, string? nativeDeviceId)
    {
        if (device.Id.Equals(stableDeviceId, StringComparison.Ordinal))
        {
            return true;
        }

        return !string.IsNullOrEmpty(nativeDeviceId) &&
            !string.IsNullOrEmpty(device.NativeDeviceId) &&
            device.NativeDeviceId.Equals(nativeDeviceId, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// The window the shell waits for a native-connected camera to deliver its
    /// FIRST frame (signalPresent) before giving up and falling back to the
    /// WinUI MediaCapture bridge. Longer than the core's no-first-frame watchdog
    /// (kUvcNoFirstFrameTimeoutMs) so the core marks the device "error" (and
    /// releases it) first, which the shell then observes as FallBack.
    /// </summary>
    public const int FirstFrameTimeoutMs = 6000;

    public enum FirstFrameOutcome
    {
        /// <summary>Still connected, no frame yet, within the timeout — keep polling.</summary>
        Waiting,

        /// <summary>A real frame arrived (signalPresent) — commit to the native path.</summary>
        Confirmed,

        /// <summary>
        /// The device errored, disappeared, or the timeout elapsed with no
        /// frame — release native and fall back to the managed bridge.
        /// </summary>
        FallBack,
    }

    /// <summary>
    /// Pure decision for the shell's first-frame confirmation loop. The device
    /// is CONFIRMED once the core reports a real frame (signalPresent); it FALLS
    /// BACK when the device is gone from the core's list, has gone to a non-
    /// connected state (the native watchdog fired / open failed), or the wait
    /// window has elapsed without a frame. Otherwise keep waiting.
    /// </summary>
    public static FirstFrameOutcome EvaluateFirstFrame(
        NativeCaptureDeviceStatus? device,
        int elapsedMs,
        int timeoutMs = FirstFrameTimeoutMs)
    {
        if (device is not null && device.SignalPresent)
        {
            return FirstFrameOutcome.Confirmed;
        }

        if (device is null ||
            !device.ConnectionState.Equals("connected", StringComparison.Ordinal))
        {
            // Dropped from the list, or the core downgraded it out of "connected"
            // (error / detected) — the native reader is not going to deliver.
            return FirstFrameOutcome.FallBack;
        }

        return elapsedMs >= timeoutMs ? FirstFrameOutcome.FallBack : FirstFrameOutcome.Waiting;
    }
}
