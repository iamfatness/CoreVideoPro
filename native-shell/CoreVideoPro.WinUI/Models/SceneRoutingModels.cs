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
    // "none" by default: borders are opt-in styling. The old "accent" default
    // baked a studio-green frame around every default route into the composed
    // program — which the virtual camera, recordings, and streams all inherit.
    public const string BorderStyle = "none";
    public const string BorderColor = "#44C1A1";
    public const double BorderThickness = 2;
    public const double SourceScale = 1;
    public const double SourceOffsetX = 0;
    public const double SourceOffsetY = 0;
}

public sealed class SourceRoute
{
    public required string Id { get; init; }
    public SourceRouteMode Mode { get; set; }
    public string? ParticipantId { get; set; }
    public string? CaptureDeviceId { get; set; }
    public int? ShowInputSlotNumber { get; set; }
    // Role-targeted route (scenes redesign R1): resolves at sync time to
    // whichever participant currently holds the production role, so saved
    // scenes ("Host + Reader") stay valid no matter who joins.
    public string? ProductionRoleId { get; set; }
    public int? SpotlightIndex { get; set; }
    public SourceAudioRole AudioRole { get; set; }
    public NormalizedCanvasRect? CanvasRect { get; set; }
    public string FitMode { get; set; } = SourceRouteVisualDefaults.FitMode;
    public string BorderStyle { get; set; } = SourceRouteVisualDefaults.BorderStyle;
    public string BorderColor { get; set; } = SourceRouteVisualDefaults.BorderColor;
    public double BorderThickness { get; set; } = SourceRouteVisualDefaults.BorderThickness;
    public double SourceScale { get; set; } = SourceRouteVisualDefaults.SourceScale;
    public double SourceOffsetX { get; set; } = SourceRouteVisualDefaults.SourceOffsetX;
    public double SourceOffsetY { get; set; } = SourceRouteVisualDefaults.SourceOffsetY;
    // Per-layer opacity 0..1 (scenes redesign S1). The compositor always
    // supported it; the scene graph now carries it.
    public double Opacity { get; set; } = 1.0;
    public bool SourceFramingModified { get; set; }
    public ColorGrade? ColorGrade { get; set; }
    public int ZIndex { get; set; }

    public SourceRoute Clone() =>
        new()
        {
            Id = Id,
            Mode = Mode,
            ParticipantId = ParticipantId,
            CaptureDeviceId = CaptureDeviceId,
            ShowInputSlotNumber = ShowInputSlotNumber,
            ProductionRoleId = ProductionRoleId,
            SpotlightIndex = SpotlightIndex,
            AudioRole = AudioRole,
            CanvasRect = CanvasRect?.Clone(),
            FitMode = FitMode,
            BorderStyle = BorderStyle,
            BorderColor = BorderColor,
            BorderThickness = BorderThickness,
            SourceScale = SourceScale,
            SourceOffsetX = SourceOffsetX,
            SourceOffsetY = SourceOffsetY,
            Opacity = Opacity,
            SourceFramingModified = SourceFramingModified,
            ColorGrade = ColorGrade,
            ZIndex = ZIndex
        };
}

public sealed class RouteSelectOption
{
    public required string Value { get; init; }
    public required string Label { get; init; }
}
