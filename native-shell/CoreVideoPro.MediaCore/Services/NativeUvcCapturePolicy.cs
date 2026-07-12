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

            if (device.Id.Equals(stableDeviceId, StringComparison.Ordinal))
            {
                return device;
            }

            if (!string.IsNullOrEmpty(nativeDeviceId) &&
                !string.IsNullOrEmpty(device.NativeDeviceId) &&
                device.NativeDeviceId.Equals(nativeDeviceId, StringComparison.OrdinalIgnoreCase))
            {
                return device;
            }
        }

        return null;
    }
}
