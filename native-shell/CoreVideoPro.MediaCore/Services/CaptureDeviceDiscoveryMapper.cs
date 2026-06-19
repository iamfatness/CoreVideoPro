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

        return "windows";
    }
}