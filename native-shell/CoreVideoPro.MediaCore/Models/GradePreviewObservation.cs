namespace CoreVideoPro.MediaCore.Models;

public sealed record GradePreviewObservation
{
    public string Type { get; init; } = "";
    public string InstanceId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public long Revision { get; init; }
    public long AppliedRevision { get; init; }
    public long SourceAgeMs { get; init; }
    public ulong SourceEpoch { get; init; }
    public long SourceFrameId { get; init; }
    public long CaptureTimestamp100ns { get; init; }
    public string Status { get; init; } = "unavailable";
    public string Reason { get; init; } = "";
    public ProgramSharedTexture? Texture { get; init; }
    public GradeScopeObservation? Scopes { get; init; }
}
public sealed record GradeScopeObservation
{
    public long CompletionObservedAtUnixMs { get; init; }
    public int View { get; init; }
    public string Reason { get; init; } = "";
    public long SourceAgeMs { get; init; }
    public string Status { get; init; } = "unavailable";
    public long Revision { get; init; }
    public ulong SourceEpoch { get; init; }
    public long SourceFrameId { get; init; }
    public long CaptureTimestamp100ns { get; init; }
    public bool Original { get; init; }
    public int SampleWidth { get; init; }
    public int SampleHeight { get; init; }
    public int SampleCount { get; init; }
    public string ColorSpace { get; init; } = "";
    public string Units { get; init; } = "";
    public ProgramSharedTexture? Texture { get; init; }
}
