namespace CoreVideoPro.WinUI.Models;

public enum SourceRouteMode
{
    Fixed,
    CaptureDevice,
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

public static class SourceRouteVisualDefaults
{
    public const string FitMode = "fill";
    public const string BorderStyle = "accent";
    public const string BorderColor = "#44C1A1";
    public const double BorderThickness = 2;
}

public sealed class SourceRoute
{
    public required string Id { get; init; }
    public SourceRouteMode Mode { get; set; }
    public string? ParticipantId { get; set; }
    public string? CaptureDeviceId { get; set; }
    public int? ShowInputSlotNumber { get; set; }
    public int? SpotlightIndex { get; set; }
    public SourceAudioRole AudioRole { get; set; }
    public NormalizedCanvasRect? CanvasRect { get; set; }
    public string FitMode { get; set; } = SourceRouteVisualDefaults.FitMode;
    public string BorderStyle { get; set; } = SourceRouteVisualDefaults.BorderStyle;
    public string BorderColor { get; set; } = SourceRouteVisualDefaults.BorderColor;
    public double BorderThickness { get; set; } = SourceRouteVisualDefaults.BorderThickness;
    public int ZIndex { get; set; }

    public SourceRoute Clone() =>
        new()
        {
            Id = Id,
            Mode = Mode,
            ParticipantId = ParticipantId,
            CaptureDeviceId = CaptureDeviceId,
            ShowInputSlotNumber = ShowInputSlotNumber,
            SpotlightIndex = SpotlightIndex,
            AudioRole = AudioRole,
            CanvasRect = CanvasRect?.Clone(),
            FitMode = FitMode,
            BorderStyle = BorderStyle,
            BorderColor = BorderColor,
            BorderThickness = BorderThickness,
            ZIndex = ZIndex
        };
}

public sealed class RouteSelectOption
{
    public required string Value { get; init; }
    public required string Label { get; init; }
}
