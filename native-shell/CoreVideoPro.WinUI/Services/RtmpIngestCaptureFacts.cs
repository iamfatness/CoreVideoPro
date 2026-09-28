using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Maps the existing core capture-device snapshot into RTMP operator status.</summary>
public static class RtmpIngestCaptureFacts
{
    public sealed record Fact(string DeviceId, string ConnectionState, bool SignalPresent,
        int Width, int Height, int FrameRate);

    public static IReadOnlyList<Fact> Read(IReadOnlyList<NativeCaptureDeviceStatus> devices)
    {
        return devices
            .Where(device => device.Id.StartsWith("rtmp-ingest-", StringComparison.Ordinal))
            .Select(device => new Fact(device.Id, device.ConnectionState, device.SignalPresent,
                device.Width, device.Height, device.FrameRate))
            .ToList();
    }

    public static bool Apply(CaptureDevice device, Fact fact)
    {
        var state = fact.ConnectionState == "failed" ? CaptureConnectionState.Error :
            fact.SignalPresent || device.ConnectionState == CaptureConnectionState.Connected
                ? CaptureConnectionState.Connected : CaptureConnectionState.Detected;
        var width = fact.SignalPresent ? fact.Width : 0;
        var height = fact.SignalPresent ? fact.Height : 0;
        var frameRate = fact.SignalPresent ? fact.FrameRate : 0;
        var changed = device.ConnectionState != state || device.SignalPresent != fact.SignalPresent ||
            device.ObservedFrameWidth != width || device.ObservedFrameHeight != height ||
            device.ObservedFrameRate != frameRate;
        if (!changed) return false;
        device.ConnectionState = state;
        device.SignalPresent = fact.SignalPresent;
        if (fact.SignalPresent) device.ApplyFrameTelemetry(width, height, frameRate);
        else
        {
            device.ObservedFrameWidth = 0;
            device.ObservedFrameHeight = 0;
            device.ObservedFrameRate = 0;
        }
        return true;
    }
}
