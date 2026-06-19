namespace CoreVideoPro.MediaCore.Models;

public sealed record FirstFrameValidationEvidence
{
    public bool ZoomVideoFrameObserved { get; init; }
    public bool ProgramPreviewFrameObserved { get; init; }
    public bool ParticipantFreshnessObserved { get; init; }
    public bool AudioFreshnessObserved { get; init; }
    public string MeetingState { get; init; } = "idle";
    public int ParticipantCount { get; init; }
    public int LiveZoomFrameCount { get; init; }
    public int ProgramFrameCount { get; init; }
    public int AudioPacketCount { get; init; }
    public int AudioActiveParticipantCount { get; init; }
    public string? LastZoomParticipantId { get; init; }
    public int? LastZoomFrameId { get; init; }
    public int? LastZoomFrameWidth { get; init; }
    public int? LastZoomFrameHeight { get; init; }
    public double? FirstZoomFrameElapsedMs { get; init; }
    public int? LastProgramFrameNumber { get; init; }
    public IReadOnlyList<string> Evidence { get; init; } = [];
    public IReadOnlyList<string> Missing { get; init; } = [];

    public bool Ready =>
        ZoomVideoFrameObserved &&
        ProgramPreviewFrameObserved &&
        ParticipantFreshnessObserved &&
        AudioFreshnessObserved;
}
