namespace CoreVideoPro.WinUI.Models;

public enum ZoomSdkRuntimePlatform
{
    MacOs,
    Windows
}

public enum ZoomSdkReadinessStatus
{
    Ready,
    Warning,
    Blocked
}

public sealed class ZoomSdkReadinessInput
{
    public ZoomSdkRuntimePlatform Platform { get; init; } = ZoomSdkRuntimePlatform.Windows;
    public bool SdkRuntimePresent { get; init; }
    public string? SdkVersion { get; init; }
    public bool AppKeyPresent { get; init; }
    public bool OauthConfigured { get; init; }
    public bool JwtBrokerConfigured { get; init; }
    public bool RawVideoEnabled { get; init; }
    public bool RawAudioEnabled { get; init; }
    public bool RawShareEnabled { get; init; }
    public string? PackagingPath { get; init; }
}

public sealed class ZoomSdkReadinessCheck
{
    public required string Id { get; init; }
    public ZoomSdkReadinessStatus Status { get; init; }
    public required string Label { get; init; }
    public required string Detail { get; init; }

    public bool ShowDetail => Status != ZoomSdkReadinessStatus.Ready;
}

public sealed class ZoomSdkReadinessReport
{
    public ZoomSdkReadinessStatus Status { get; init; }
    public ZoomSdkRuntimePlatform Platform { get; init; }
    public required string SdkVersion { get; init; }
    public required IReadOnlyList<ZoomSdkReadinessCheck> Checks { get; init; }
    public required IReadOnlyList<string> Blockers { get; init; }
    public required IReadOnlyList<string> Warnings { get; init; }
    public required string Summary { get; init; }
}

public enum ZoomMeetingState
{
    Idle,
    Joining,
    InMeeting,
    Reconnecting,
    Error
}