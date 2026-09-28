using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

public static class SrtIngestStatusPolicy
{
    public static string Label(NativeCaptureDeviceStatus? status)
    {
        if (status is null) return "Engine off or awaiting SRT status";

        var state = string.IsNullOrWhiteSpace(status.ConnectionState) ? "unknown" : status.ConnectionState;
        var age = status.LastFrameAgeMs is { } frameAge
            ? $"last frame {frameAge} ms ago"
            : "no decoded frame yet";
        var codec = status.CodecDecodeErrors ?? 0;
        var packet = status.PacketDecodeErrors ?? 0;
        var process = status.DecoderFailures ?? 0;
        var rtt = status.RttMs is { } measured
            ? $"RTT {measured:0.#} ms"
            : "RTT unavailable (FFmpeg owns socket)";
        return $"{state} · {age} · codec errors {codec} · packet errors {packet} · decoder starts failed {process} · {rtt}";
    }
}
