namespace CoreVideoPro.Control;

/// <summary>One Show Input's feedback state.</summary>
public sealed record ControlInputState(int Slot, bool InShow, string Kind, string? SourceId, string Name);

/// <summary>Flat, serializable feedback snapshot for control-surface button state (Companion
/// feedbacks, WS clients). Built by the WinUI adapter from ViewModel state + the core snapshot,
/// and emitted (coalesced) on change. Field names are the stable feedback contract — keep them
/// aligned with the OSC feedback address space (see <see cref="ControlManifest"/>).</summary>
public sealed record ControlState
{
    public bool Recording { get; init; }
    public bool Streaming { get; init; }
    public bool EngineOn { get; init; }
    public bool CanTake { get; init; }
    public bool LowerThirdOnAir { get; init; }

    public string ZoomStatus { get; init; } = string.Empty;
    public string EngineStatus { get; init; } = string.Empty;
    public string CommandStatus { get; init; } = string.Empty;

    public string ActiveSceneId { get; init; } = string.Empty;
    public string PreviewSceneId { get; init; } = string.Empty;
    public string ViewMode { get; init; } = string.Empty;

    public string MultiviewLayoutMode { get; init; } = string.Empty;
    public int MultiviewTileCount { get; init; }

    public bool AutomationOn { get; init; }
    public bool AutoTake { get; init; }
    public bool AutoAssignInputs { get; init; }
    public bool AutoLowerThirds { get; init; }
    public bool AutoCaptions { get; init; }

    public bool AudioMonitorOn { get; init; }
    public double AudioMonitorVolume { get; init; }
    public bool MasterLimiterOn { get; init; }

    public IReadOnlyList<ControlInputState> Inputs { get; init; } = System.Array.Empty<ControlInputState>();

    public static ControlState Empty { get; } = new();
}
