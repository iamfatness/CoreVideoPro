namespace CoreVideoPro.Control;

/// <summary>One Show Input's feedback state.</summary>
public sealed record ControlInputState(int Slot, bool InShow, string Kind, string? SourceId, string Name);

/// <summary>Live native meter state for one independently mixed source.</summary>
public sealed record ControlAudioSourceState(
    string SourceId,
    string Name,
    int Level,
    double PeakDbfs,
    double RmsDbfs,
    bool Muted,
    string Status);

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
    // Native observations are nullable when no running core snapshot is available.
    // The original fields above remain operator/local state for compatibility.
    public string? NativeActiveSceneId { get; init; }
    // Actual last-rendered frame evidence; may lag acknowledged scene state.
    public string? NativeRenderedSceneId { get; init; }
    public string? NativeRenderPlanId { get; init; }
    public IReadOnlyList<ControlProgramVideoSource>? NativeProgramVideoSources { get; init; }
    public string? NativeLowerThirdSourceId { get; init; }
    public string? NativePreviewSceneId { get; init; }
    public string? NativeLowerThirdPhase { get; init; }
    public bool? NativeLowerThirdVisible { get; init; }
    public int? NativeProgramFrameCount { get; init; }
    public System.Text.Json.JsonElement? NativeProgramBuffer { get; init; }
    public int? ProgramBufferRequestedFrames { get; init; }
    public int? ProgramBufferSessionRequestedFrames { get; init; }
    public bool? ProgramBufferRestartRequired { get; init; }
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
    public string ZoomAudioMode { get; init; } = string.Empty;
    public bool MasterLimiterOn { get; init; }
    public bool MasteringOn { get; init; }
    public int MasteringTarget { get; init; }
    public int AudioSourceCount { get; init; }
    public double ProgramTruePeakDbfs { get; init; } = -120;
    public double ProgramLoudnessLufs { get; init; } = -120;
    public string AudioValidationSummary { get; init; } = string.Empty;
    public string VstHostStatus { get; init; } = string.Empty;
    public int VstPluginCount { get; init; }
    public string VstHostSummary { get; init; } = string.Empty;

    public bool VirtualCameraOn { get; init; }
    public string VirtualCameraStatus { get; init; } = string.Empty;
    public string VirtualCameraRawStatus { get; init; } = string.Empty;

    public IReadOnlyList<ControlInputState> Inputs { get; init; } = System.Array.Empty<ControlInputState>();
    public IReadOnlyList<ControlAudioSourceState> AudioSources { get; init; } = System.Array.Empty<ControlAudioSourceState>();

    public static ControlState Empty { get; } = new();
}

public sealed record ControlProgramVideoSource(string LayerId, string SourceId, string ParticipantId, string Kind);
