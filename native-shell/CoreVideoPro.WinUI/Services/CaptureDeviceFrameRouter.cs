namespace CoreVideoPro.WinUI.Services;

public sealed class CaptureDeviceFrame
{
    public required string DeviceId { get; init; }
    public required byte[] Bgra { get; init; }
    public required int Width { get; init; }
    public required int Height { get; init; }
    public required int FrameId { get; init; }
    public long TimestampMs { get; init; } = Environment.TickCount64;
}

public static class CaptureDeviceFrameRouter
{
    public static event Action<CaptureDeviceFrame>? FrameReceived;

    public static void Publish(CaptureDeviceFrame frame) => FrameReceived?.Invoke(frame);
}
