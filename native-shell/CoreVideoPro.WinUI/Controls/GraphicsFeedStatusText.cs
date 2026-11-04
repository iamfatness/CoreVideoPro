using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Controls;

internal static class GraphicsFeedStatusText
{
    internal static string Describe(GraphicsFeedStatus status, GraphicsDataSource? source)
    {
        var success = status.LastSuccess is { } checkedAt ? checkedAt.ToLocalTime().ToString("g") : "none this session";
        var age = source is null ? "none" :
            TimeSpan.FromSeconds(Math.Max(0, Math.Floor((DateTimeOffset.UtcNow - source.ReceivedAt).TotalSeconds))).ToString("g");
        var failure = status.Failure switch
        {
            "timeout" => "Request timed out",
            "invalid-data" => "Invalid response or unsafe destination",
            "rejected" => "Dataset rejected or workspace busy",
            "credentials-required" => "Authorize this feed on this computer",
            "unavailable" => "Feed unavailable",
            _ => null
        };
        var retry = status.NextCheck is { } due ? $"\nNext check · {due.ToLocalTime():T}" : "";
        return $"Feed {status.State}\nLast successful check · {success}\nAccepted data age · {age}" +
            (failure is null ? "" : $"\n{failure}") + retry;
    }
}
