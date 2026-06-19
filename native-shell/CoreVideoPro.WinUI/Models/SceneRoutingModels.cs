namespace CoreVideoPro.WinUI.Models;

public enum SourceRouteMode
{
    Fixed,
    ActiveSpeaker,
    Spotlight,
    ScreenShare,
    None
}

public enum SourceAudioRole
{
    Mix,
    Isolated,
    Audience
}

public sealed class SourceRoute
{
    public required string Id { get; init; }
    public SourceRouteMode Mode { get; set; }
    public string? ParticipantId { get; set; }
    public int? SpotlightIndex { get; set; }
    public SourceAudioRole AudioRole { get; set; }
    public NormalizedCanvasRect? CanvasRect { get; set; }
    public int ZIndex { get; set; }

    public SourceRoute Clone() =>
        new()
        {
            Id = Id,
            Mode = Mode,
            ParticipantId = ParticipantId,
            SpotlightIndex = SpotlightIndex,
            AudioRole = AudioRole,
            CanvasRect = CanvasRect?.Clone(),
            ZIndex = ZIndex
        };
}

public sealed class RouteSelectOption
{
    public required string Value { get; init; }
    public required string Label { get; init; }
}