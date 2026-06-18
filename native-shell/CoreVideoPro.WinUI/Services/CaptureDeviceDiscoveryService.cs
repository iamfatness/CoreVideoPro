using System.Security.Cryptography;
using System.Text;
using CoreVideoPro.WinUI.Models;
using Windows.Devices.Enumeration;

namespace CoreVideoPro.WinUI.Services;

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

    public static CaptureDevice MapDiscoveredDevice(string deviceSymbolicLinkId, string friendlyName)
    {
        return new CaptureDevice
        {
            Id = CreateStableDeviceId(deviceSymbolicLinkId),
            Vendor = DetectVendor(friendlyName),
            Name = friendlyName,
            Inputs = [new CaptureDeviceInput { Id = "default", Label = "Default" }],
            SelectedInputId = "default",
            Width = 0,
            Height = 0,
            FrameRate = 0,
            ConnectionState = CaptureConnectionState.Detected,
            SignalPresent = false,
            AudioSyncOffsetMs = 0
        };
    }
}

public sealed class CaptureDeviceDiscoveryService
{
    public async Task<IReadOnlyList<CaptureDevice>> DiscoverDevicesAsync()
    {
        try
        {
            var deviceInfos = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture)
                .AsTask()
                .ConfigureAwait(false);

            return deviceInfos
                .Where(info => !string.IsNullOrWhiteSpace(info.Name))
                .GroupBy(info => info.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => CaptureDeviceDiscoveryMapper.MapDiscoveredDevice(group.Key, group.First().Name))
                .OrderBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }
        catch
        {
            return [];
        }
    }

    public IReadOnlyList<CaptureDevice> DiscoverDevices() =>
        DiscoverDevicesAsync().GetAwaiter().GetResult();
}