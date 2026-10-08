namespace CoreVideoPro.MediaCore.Models;

public sealed record GradePreviewObservation
{
    public string Type { get; init; } = "";
    public string InstanceId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public long Revision { get; init; }
    public ulong SourceEpoch { get; init; }
    public long SourceFrameId { get; init; }
    public long CaptureTimestamp100ns { get; init; }
    public string Status { get; init; } = "unavailable";
    public string Reason { get; init; } = "";
    public ProgramSharedTexture? Texture { get; init; }
}
