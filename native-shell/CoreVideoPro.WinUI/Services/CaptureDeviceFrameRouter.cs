namespace CoreVideoPro.WinUI.Services;

public sealed class CaptureDeviceFrame
{
    public required string DeviceId { get; init; }
    public required byte[] Bgra { get; init; }
    public required int Width { get; init; }
    public required int Height { get; init; }
    public int NaturalSourceWidth { get; init; }
    public int NaturalSourceHeight { get; init; }
    public int Fps { get; init; }
    public required int FrameId { get; init; }
    public long TimestampMs { get; init; } = Environment.TickCount64;
}

public static class CaptureDeviceFrameRouter
{
    public static event Action<CaptureDeviceFrame>? FrameReceived;

    public static void Publish(CaptureDeviceFrame frame)
    {
        var handlers = FrameReceived?.GetInvocationList();
        if (handlers is null)
        {
            return;
        }

        foreach (var handler in handlers)
        {
            try
            {
                ((Action<CaptureDeviceFrame>)handler)(frame);
            }
            catch (Exception ex)
            {
                LaunchLog.Write($"capture: frame subscriber failed {handler.Method.DeclaringType?.Name}.{handler.Method.Name}: {ex.GetType().Name}: {ex.Message}");
            }
        }
    }
}
