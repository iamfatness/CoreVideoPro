namespace CoreVideoPro.WinUI.Services;

public sealed record StreamingProfileDefinition(
    string Id,
    string Label,
    string Summary,
    string Resolution,
    string Fps,
    string VideoCodec,
    double TargetBitrateMbps,
    int AudioBitrateKbps,
    double KeyframeIntervalSeconds,
    string RateControl,
    string H264Profile,
    int BFrames,
    string Protocol = "rtmps",
    bool AllowEnhancedRtmp = false);

/// <summary>
/// Safe starting points derived from the platforms' published encoder guidance.
/// The Custom entry deliberately carries no defaults: selecting it preserves the
/// operator's current endpoint and encoder values for any standards-compliant
/// RTMP or RTMPS service.
/// </summary>
public static class StreamingProfileCatalog
{
    public const string CustomProfileId = "custom";

    public static IReadOnlyList<StreamingProfileDefinition> Profiles { get; } =
    [
        new(
            CustomProfileId,
            "Custom RTMP / RTMPS",
            "Use any RTMP endpoint and tune every encoder setting manually.",
            string.Empty,
            string.Empty,
            string.Empty,
            0,
            0,
            0,
            string.Empty,
            string.Empty,
            0),
        new(
            "youtube-1080p60",
            "YouTube 1080p60",
            "YouTube recommended H.264 starting point: 12 Mbps, 2-second keyframes, AAC 128 kbps.",
            "1920x1080", "60", "h264", 12, 128, 2, "cbr", "high", 2),
        new(
            "youtube-1080p30",
            "YouTube 1080p30",
            "YouTube recommended H.264 starting point: 10 Mbps, 2-second keyframes, AAC 128 kbps.",
            "1920x1080", "30", "h264", 10, 128, 2, "cbr", "high", 2),
        new(
            "twitch-1080p60",
            "Twitch 1080p60",
            "Twitch maximum-quality starting point: 6 Mbps CBR, 2-second keyframes, AAC 160 kbps.",
            "1920x1080", "60", "h264", 6, 160, 2, "cbr", "high", 2),
        new(
            "twitch-1080p30",
            "Twitch 1080p30",
            "Twitch recommended starting point: 4.5 Mbps CBR, 2-second keyframes, AAC 160 kbps.",
            "1920x1080", "30", "h264", 4.5, 160, 2, "cbr", "high", 2),
        new(
            "facebook-1080p30",
            "Facebook Live 1080p30",
            "Facebook upper recommended range: 6 Mbps, 2-second keyframes, AAC 128 kbps.",
            "1920x1080", "30", "h264", 6, 128, 2, "cbr", "high", 2),
        new(
            "linkedin-720p30",
            "LinkedIn Live 720p30",
            "LinkedIn recommended starting point: 3.5 Mbps, 2-second keyframes, H.264 Baseline, no B-frames.",
            "1280x720", "30", "h264", 3.5, 128, 2, "cbr", "baseline", 0)
    ];

    public static StreamingProfileDefinition Find(string? id) =>
        Profiles.FirstOrDefault(profile => string.Equals(profile.Id, id, StringComparison.OrdinalIgnoreCase)) ??
        Profiles[0];
}
