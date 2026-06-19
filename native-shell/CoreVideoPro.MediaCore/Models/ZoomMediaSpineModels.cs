namespace CoreVideoPro.MediaCore.Models;

public sealed class ZoomMediaSpineParticipant
{
    public string SdkUserId { get; init; } = string.Empty;
    public string DisplayName { get; init; } = string.Empty;
    public string Role { get; init; } = "guest";
    public bool VideoOn { get; init; } = true;
    public bool Muted { get; init; }
    public bool Talking { get; init; }
    public bool SharingScreen { get; init; }
    public int AudioLevel { get; init; }
    public string NetworkQuality { get; init; } = "good";
}

public sealed class ZoomMediaSpineSubscription
{
    public string ParticipantId { get; init; } = string.Empty;
    public string Kind { get; init; } = string.Empty;
    public string Purpose { get; init; } = string.Empty;
    public int Priority { get; init; }
    public string SubscriptionId { get; init; } = string.Empty;
    public string? DisplayName { get; init; }
    public string Status { get; init; } = "subscribed";
    public string LastResultCode { get; init; } = "ok";
    public int FramesReceived { get; init; }
    public int AudioPacketsReceived { get; init; }
    public double FirstFrameAtMs { get; init; } = -1;
    public double FirstFrameDelayMs { get; init; } = -1;
    public double LastFrameAtMs { get; init; } = -1;
    public string? Warning { get; init; }
}

public sealed class ZoomMediaSpineNativeSnapshot
{
    public string MeetingState { get; init; } = "idle";
    public string SdkVersion { get; init; } = string.Empty;
    public int ParticipantCount { get; init; }
    public string? ActiveSpeakerId { get; init; }
    public string? ScreenShareParticipantId { get; init; }
    public IReadOnlyList<ZoomMediaSpineParticipant> Participants { get; init; } = [];
    public IReadOnlyList<ZoomMediaSpineSubscription> Subscriptions { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
    public IReadOnlyList<string> Events { get; init; } = [];
}