namespace CoreVideoPro.WinUI.Services;

/// <summary>Requested camera ceiling, separate from negotiated source format.</summary>
public static class ZoomCameraResolutionPreference
{
    public const string Default = "1080p";
    public static IReadOnlyList<string> Choices { get; } = ["360p", "720p", "1080p"];

    public static string Normalize(string? choice) => choice switch
    {
        "360p" => "360p",
        "720p" => "720p",
        _ => Default
    };

    public static int ToSdkTier(string? choice) => Normalize(choice) switch
    {
        "360p" => 0,
        "720p" => 1,
        _ => 2
    };

    public static string FromSdkTier(int tier) => tier switch
    {
        0 => "360p",
        1 => "720p",
        _ => Default
    };

    public static (int Width, int Height) Dimensions(string? choice) => Normalize(choice) switch
    {
        "360p" => (640, 360),
        "720p" => (1280, 720),
        _ => (1920, 1080)
    };

    public static string Status(string? preferred, string? applied, bool inMeeting)
    {
        var wanted = Normalize(preferred);
        var current = Normalize(applied);
        return !inMeeting
            ? $"Preferred max {wanted}. Applies on the next Zoom join; Format shows actual delivery."
            : wanted == current
                ? $"Requested max {current} this meeting. Format shows what Zoom actually delivers."
                : $"Current request {current}; {wanted} pending for next Zoom join.";
    }
}
