using System.Security.Cryptography;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Pure mapping helpers for WinUI video-capture enumeration — testable without WinRT.
/// </summary>
public static class CaptureDeviceDiscoveryMapper
{
    public static string CreateStableDeviceId(string symbolicLinkId)
    {
        if (string.IsNullOrWhiteSpace(symbolicLinkId))
        {
            return Guid.NewGuid().ToString("N");
        }

        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(symbolicLinkId));
        return Convert.ToHexString(hash, 0, 8).ToLowerInvariant();
    }

    public static string DetectVendor(string friendlyName)
    {
        var normalized = friendlyName.ToLowerInvariant();
        if (normalized.Contains("blackmagic", StringComparison.Ordinal) ||
            normalized.Contains("decklink", StringComparison.Ordinal))
        {
            return "blackmagic";
        }

        if (normalized.Contains("aja", StringComparison.Ordinal))
        {
            return "aja";
        }

        if (normalized.Contains("elgato", StringComparison.Ordinal) ||
            normalized.Contains("cam link", StringComparison.Ordinal) ||
            normalized.Contains("game capture", StringComparison.Ordinal) ||
            normalized.Contains("hd60", StringComparison.Ordinal) ||
            normalized.Contains("usb capture", StringComparison.Ordinal) ||
            normalized.Contains("hdmi capture", StringComparison.Ordinal) ||
            normalized.Contains("uvc", StringComparison.Ordinal))
        {
            return "uvc";
        }

        return "windows";
    }

    public static bool IsVirtualCameraName(string friendlyName)
    {
        var normalized = friendlyName.ToLowerInvariant();
        return normalized.Contains("virtual camera", StringComparison.Ordinal) ||
            normalized.Contains("virtualcam", StringComparison.Ordinal) ||
            normalized.Contains("obs virtual", StringComparison.Ordinal) ||
            normalized.Contains("ndi video", StringComparison.Ordinal);
    }

    public static int OperatorSelectionPriority(string friendlyName, string vendor)
    {
        if (IsVirtualCameraName(friendlyName))
        {
            return 20;
        }

        return vendor.ToLowerInvariant() switch
        {
            "blackmagic" => 0,
            "aja" => 1,
            "uvc" => 2,
            "windows" => 8,
            _ => 10
        };
    }
}
