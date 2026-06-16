using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Models;

public enum OutputHealthKind
{
    Idle,
    Good,
    Warning
}

public static class TransportFormatting
{
    public const int DemoElapsedSeconds = 1727;
    public const int DemoEncoderLoadPercent = 18;
    public const int DemoMasterLevel = 72;
    public const double DemoBitrateMbps = 6;
    public const double DemoAvailableDiskGb = 247.3;

    public static string FormatElapsed(int seconds)
    {
        var safeSeconds = Math.Max(0, seconds);
        var minutes = (safeSeconds / 60).ToString("D2");
        var remaining = (safeSeconds % 60).ToString("D2");
        return $"{minutes}:{remaining}";
    }

    public static string ShortResolutionLabel(string resolution, int fps)
    {
        var parts = resolution.Split('x', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 2 && int.TryParse(parts[1], out var height))
        {
            var tier = height >= 2160 ? "4K" : $"{height}p";
            return $"{tier}{fps}";
        }

        return $"{resolution} {fps}fps";
    }

    public static string HealthLabel(OutputHealthKind health) => health switch
    {
        OutputHealthKind.Good => "Good",
        OutputHealthKind.Warning => "Warning",
        _ => "Idle"
    };

    public static OutputHealthKind MapNetworkHealth(string? status)
    {
        if (string.IsNullOrWhiteSpace(status))
        {
            return OutputHealthKind.Idle;
        }

        return status.ToLowerInvariant() switch
        {
            "warning" => OutputHealthKind.Warning,
            "live" or "good" or "excellent" => OutputHealthKind.Good,
            _ => OutputHealthKind.Idle
        };
    }

    public static SolidColorBrush HealthBrush(OutputHealthKind health) => health switch
    {
        OutputHealthKind.Good => new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151)),
        OutputHealthKind.Warning => new SolidColorBrush(Windows.UI.Color.FromArgb(255, 240, 168, 92)),
        _ => new SolidColorBrush(Windows.UI.Color.FromArgb(255, 108, 122, 114))
    };

    public static string PercentLabel(int value) => $"{Math.Clamp(value, 0, 100)}%";

    public static int MemoryLoadFromEncoder(int encoderLoad) =>
        Math.Min(100, (int)Math.Round(encoderLoad * 0.7 + 10));

    public static int DiskLoadFromEncoder(int encoderLoad) =>
        Math.Min(100, (int)Math.Round(encoderLoad * 0.4 + 20));

    public static string FrameDropsLabel(int droppedFrames, int totalFrames)
    {
        if (totalFrames <= 0)
        {
            return $"{droppedFrames} (0.0%)";
        }

        var percent = droppedFrames * 100d / totalFrames;
        return $"{droppedFrames} ({percent:0.0}%)";
    }
}