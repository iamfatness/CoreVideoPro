namespace CoreVideoPro.MediaCore.Models;

/// <summary>
/// REDACTED diagnostics + crash/recovery snapshot for support escalation.
/// C# port of the React shell's SupportBundle (src/domain/production.ts).
/// No secrets (stream keys, endpoints, URLs with credentials) are emitted.
/// </summary>
public sealed record SupportBundle
{
    public required string Id { get; init; }
    public required string CreatedAt { get; init; }
    public required SupportBundleAppSummary App { get; init; }
    public required string SummaryText { get; init; }
    public IReadOnlyList<string> TriageLines { get; init; } = [];
    public IReadOnlyList<SupportBundleOutputDestinationSummary> Outputs { get; init; } = [];
    public SupportBundleMediaCore? MediaCore { get; init; }
    public SupportBundleRuntime? Runtime { get; init; }
    public SupportBundleCrashRecovery? CrashRecovery { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed record SupportBundleAppSummary
{
    public required string Name { get; init; }
    public required string Version { get; init; }
    public required string Platform { get; init; }
}

public sealed record SupportBundleRuntime
{
    public required string Status { get; init; }
    public required string Label { get; init; }
    public required string Host { get; init; }
    public required string Platform { get; init; }
    public int RestartCount { get; init; }
    public bool Recovering { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed record SupportBundleCrashRecovery
{
    public IReadOnlyList<SupportBundleCrashEvent> Events { get; init; } = [];
}

public sealed record SupportBundleCrashEvent
{
    public required string At { get; init; }
    public int? ExitCode { get; init; }
    public int RestartCount { get; init; }
}

public sealed record SupportBundleOutputDestinationSummary
{
    public required string Id { get; init; }
    public string Name { get; init; } = string.Empty;
    public string Protocol { get; init; } = "rtmp";
    public bool Enabled { get; init; }
    public bool Active { get; init; }
    public string? Resolution { get; init; }
    public string? Fps { get; init; }
    public string? Codec { get; init; }
    public string? EncoderMode { get; init; }
    public double? TargetBitrateMbps { get; init; }
    public string? Format { get; init; }
    public string? Quality { get; init; }
    /// <summary>Redacted endpoint/URL — credentials and secret query params stripped.</summary>
    public string Endpoint { get; init; } = string.Empty;
    /// <summary>Always "absent" or "present-redacted" — the raw key is never emitted.</summary>
    public string StreamKey { get; init; } = "absent";
}

public sealed record SupportBundleMediaCore
{
    public string? SceneId { get; init; }
    public string? RenderPlanId { get; init; }
    public required SupportBundleMediaCoreSource Source { get; init; }
    public required SupportBundleMediaCoreCompositor Compositor { get; init; }
    public required SupportBundleMediaCoreTransport Transport { get; init; }
    public required SupportBundleMediaCoreEncoder Encoder { get; init; }
    public required SupportBundleMediaCoreAudio Audio { get; init; }
    public required SupportBundleMediaCoreSenders Senders { get; init; }
    public SupportBundleMediaCoreRecording? Recording { get; init; }
    public IReadOnlyList<SupportBundleOperatorAction> OperatorActions { get; init; } = [];
    public IReadOnlyList<SupportBundleEvent> EventLog { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed record SupportBundleMediaCoreSource
{
    public required string AdapterId { get; init; }
    public required string Kind { get; init; }
    public required string Status { get; init; }
    public int SubscribedSourceCount { get; init; }
    public int DeliveredFrameCount { get; init; }
    public int LiveFrameCount { get; init; }
    public int StaleFrameCount { get; init; }
    public required string Backing { get; init; }
    public int DroppedFrameCount { get; init; }
    public int LowResolutionFrameCount { get; init; }
}

public sealed record SupportBundleMediaCoreCompositor
{
    public required string Status { get; init; }
    public int ProgramFrameCount { get; init; }
    public int DroppedFrameCount { get; init; }
    public int DegradedFrameCount { get; init; }
}

public sealed record SupportBundleMediaCoreTransport
{
    public required string Status { get; init; }
    public int? FrameNumber { get; init; }
    public double LatencyMs { get; init; }
}

public sealed record SupportBundleMediaCoreEncoder
{
    public required string Status { get; init; }
    public required string Lifecycle { get; init; }
    public int TargetCount { get; init; }
}

public sealed record SupportBundleMediaCoreAudio
{
    public required string Status { get; init; }
    public int MasterLevel { get; init; }
    public double LoudnessLufs { get; init; }
    public bool LimiterActive { get; init; }
    public int MixedFrameCount { get; init; }
    public bool MonitorEnabled { get; init; }
    public string? MonitorStatus { get; init; }
    public string? MonitorDeviceName { get; init; }
    public long MonitorFramesPlayed { get; init; }
    public int ParticipantCount { get; init; }
    public int MutedParticipantCount { get; init; }
    public int BoostedParticipantCount { get; init; }
    public int DuckedParticipantCount { get; init; }
    public int LimitedParticipantCount { get; init; }
    public int LowLevelParticipantCount { get; init; }
    public int PeakOutputLevel { get; init; }
    public int WarningCount { get; init; }
    public IReadOnlyList<string> WarningCategories { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
    public SupportBundleMediaCoreAudioRouting Routing { get; init; } = new();
    public SupportBundleMediaCoreCaptureAudio Capture { get; init; } = new();
}

public sealed record SupportBundleMediaCoreAudioRouting
{
    public string Status { get; init; } = "idle";
    public int RoutedSendCount { get; init; }
    public int RoutedSourceCount { get; init; }
    public int ProgramTapFrames { get; init; }
    public IReadOnlyList<SupportBundleMediaCoreAudioBusTap> BusTaps { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed record SupportBundleMediaCoreAudioBusTap
{
    public required string BusId { get; init; }
    public int Channels { get; init; }
    public int Frames { get; init; }
    public double PeakDbfs { get; init; }
    public double RmsDbfs { get; init; }
}

public sealed record SupportBundleMediaCoreCaptureAudio
{
    public string Status { get; init; } = "idle";
    public int SourceCount { get; init; }
    public int PairedCount { get; init; }
    public int StreamingCount { get; init; }
    public long CaptureFramesReceived { get; init; }
    public int RoutedMasterFrames { get; init; }
    public int RoutedMonitorFrames { get; init; }
    public int FallbackMonitorFrames { get; init; }
    public long MonitorFramesPlayed { get; init; }
    public IReadOnlyList<SupportBundleMediaCoreCaptureAudioSource> Sources { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed record SupportBundleMediaCoreCaptureAudioSource
{
    public required string CaptureDeviceId { get; init; }
    public string? SourceId { get; init; }
    public string? AudioDeviceName { get; init; }
    public string? AudioSourceKind { get; init; }
    public bool Embedded { get; init; }
    public int AudioSyncOffsetMs { get; init; }
    public bool Paired { get; init; }
    public bool CaptureStreaming { get; init; }
    public long CaptureFramesReceived { get; init; }
    public long EmptyPacketPolls { get; init; }
    public int CaptureSampleRate { get; init; }
    public int CaptureChannels { get; init; }
    public string? EndpointName { get; init; }
    public string? LastError { get; init; }
    public string? Warning { get; init; }
}

public sealed record SupportBundleMediaCoreSenders
{
    public required string Status { get; init; }
    public int ActiveSenderCount { get; init; }
    public IReadOnlyList<SupportBundleMediaCoreSender> Destinations { get; init; } = [];
}

public sealed record SupportBundleMediaCoreSender
{
    public required string Destination { get; init; }
    public required string Status { get; init; }
    public double? StartedAtMs { get; init; }
    public double? StoppedAtMs { get; init; }
    public int? LastFrameNumber { get; init; }
    public int FramesSent { get; init; }
    public int RetryCount { get; init; }
    public double LatencyMs { get; init; }
    public double BitrateMbps { get; init; }
    public string? Warning { get; init; }
}

public sealed record SupportBundleMediaCoreRecording
{
    public required string Status { get; init; }
    public required string WriterStatus { get; init; }
    public int TotalFramesWritten { get; init; }
    public int TotalDroppedFrames { get; init; }
    public double EstimatedDiskRateMBps { get; init; }
    public IReadOnlyList<SupportBundleMediaCoreRecordingStream> Streams { get; init; } = [];
}

public sealed record SupportBundleMediaCoreRecordingStream
{
    public required string Kind { get; init; }
    public string? ParticipantId { get; init; }
    public required string Status { get; init; }
    public int FramesWritten { get; init; }
    public int DroppedFrames { get; init; }
    public long BytesWritten { get; init; }
    public string? Warning { get; init; }
}

public sealed record SupportBundleOperatorAction
{
    public required string ActionId { get; init; }
    public required string Severity { get; init; }
    public required string Area { get; init; }
    public required string Title { get; init; }
    public required string Detail { get; init; }
    public string? Command { get; init; }
    public string? RelatedId { get; init; }
}

public sealed record SupportBundleEvent
{
    public required string EventId { get; init; }
    public double AtMs { get; init; }
    public required string Severity { get; init; }
    public required string Area { get; init; }
    public required string Title { get; init; }
    public required string Detail { get; init; }
    public string? RelatedId { get; init; }
    public string? CommandType { get; init; }
}
