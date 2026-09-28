namespace CoreVideoPro.WinUI.Services;

/// <summary>Optional local Zoom camera playout ceiling; 60 preserves every received frame.</summary>
public static class ZoomCameraFrameRatePreference
{
    public const int Default = 60;
    public static IReadOnlyList<int> Choices { get; } = [15, 24, 25, 30, 60];

    public static int Normalize(int fps) => Choices.Contains(fps) ? fps : Default;

    public static string Status(int preferred, int applied, bool inMeeting)
    {
        var wanted = Normalize(preferred);
        var current = Normalize(applied);
        return !inMeeting
            ? $"Local guest video max {wanted} fps applies on the next Zoom join. Format shows Zoom's delivered fps."
            : wanted == current
                ? $"Local guest video max {current} fps this meeting. Zoom may deliver more; Format shows actual input."
                : $"Local guest video max {current} fps now; {wanted} fps pending for next Zoom join.";
    }
}
