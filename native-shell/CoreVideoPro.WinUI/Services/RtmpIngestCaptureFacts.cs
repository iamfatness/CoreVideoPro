using System.Text.Json;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Maps the existing core capture-device snapshot into RTMP operator status.</summary>
public static class RtmpIngestCaptureFacts
{
    public sealed record Fact(string DeviceId, string ConnectionState, bool SignalPresent,
        int Width, int Height, int FrameRate);

    public static IReadOnlyList<Fact> Read(JsonElement? devices)
    {
        if (devices is not { ValueKind: JsonValueKind.Array } array) return [];
        var result = new List<Fact>();
        foreach (var item in array.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Object ||
                !item.TryGetProperty("id", out var id) ||
                id.ValueKind != JsonValueKind.String ||
                id.GetString() is not { } deviceId ||
                !deviceId.StartsWith("rtmp-ingest-", StringComparison.Ordinal)) continue;
            var state = item.TryGetProperty("connectionState", out var connection) &&
                connection.ValueKind == JsonValueKind.String ? connection.GetString() ?? "" : "";
            var signal = item.TryGetProperty("signalPresent", out var present) &&
                present.ValueKind == JsonValueKind.True;
            var width = 0;
            var height = 0;
            if (item.TryGetProperty("resolution", out var resolution) && resolution.ValueKind == JsonValueKind.Object)
            {
                width = ReadInt(resolution, "width");
                height = ReadInt(resolution, "height");
            }
            result.Add(new Fact(deviceId, state, signal, width, height, ReadInt(item, "frameRate")));
        }
        return result;
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

    private static int ReadInt(JsonElement item, string property) =>
        item.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetInt32(out var number) ? number : 0;
}
