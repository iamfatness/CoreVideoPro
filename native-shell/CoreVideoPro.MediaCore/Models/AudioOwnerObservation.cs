namespace CoreVideoPro.MediaCore.Models;

public sealed record AudioOwnerObservation
{
    public string Epoch { get; init; } = string.Empty;
    public long Generation { get; init; }
    public long Sequence { get; init; }
    public long ObservedRevision { get; init; }
    public double AgeMs { get; init; }
    public string Status { get; init; } = "unobserved";
    public long Refused { get; init; }
    public int Capacity { get; init; }
    public long BudgetBytes { get; init; }
    public long RetainedMetadataBytes { get; init; }
    public long PublishedAtNs { get; init; }
    public string ClockDomain { get; init; } = string.Empty;
    public string Version { get; init; } = string.Empty;
}
