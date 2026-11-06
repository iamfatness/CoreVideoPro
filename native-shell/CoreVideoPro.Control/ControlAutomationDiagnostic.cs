namespace CoreVideoPro.Control;

/// <summary>Immutable observation of one completed shell Auto evaluation, not a command.</summary>
public sealed record ControlAutomationDiagnostic
{
    public long Sequence { get; init; }
    public DateTimeOffset ObservedUtc { get; init; }
    public long ModeGeneration { get; init; }
    public string Mode { get; init; } = "Manual";
    public string LastAction { get; init; } = string.Empty;
    public string RecommendationSceneId { get; init; } = string.Empty;
    public int Confidence { get; init; }
    public string RecommendationReason { get; init; } = string.Empty;
    public IReadOnlyList<ControlAutomationBinding> Bindings { get; init; } = [];
    public bool BindingsTruncated { get; init; }
    public string? PendingSceneId { get; init; }
    public double PendingElapsedMs { get; init; }
    public bool TakeInFlight { get; init; }
    public bool CanTake { get; init; }
    public string PreviewSceneId { get; init; } = string.Empty;
    public string ActiveSceneId { get; init; } = string.Empty;
    public string CoreEpoch { get; init; } = string.Empty;
    public string RosterEpoch { get; init; } = string.Empty;
    public bool AutoTake { get; init; }
    public double ConfidenceThreshold { get; init; }
    public double SwitchDelaySeconds { get; init; }
    public bool PreferScreenShare { get; init; }
    public int PanelGuestThreshold { get; init; }
}

public sealed record ControlAutomationBinding(int SlotIndex, string PersonId, string SourceId);
