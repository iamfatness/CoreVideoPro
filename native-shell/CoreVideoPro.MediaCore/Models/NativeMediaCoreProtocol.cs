using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

public enum NativeMediaCoreCapability
{
    [JsonStringEnumMemberName("zoom-raw-video")] ZoomRawVideo,
    [JsonStringEnumMemberName("zoom-raw-audio")] ZoomRawAudio,
    [JsonStringEnumMemberName("gpu-compositor")] GpuCompositor,
    [JsonStringEnumMemberName("scene-graph-rendering")] SceneGraphRendering,
    [JsonStringEnumMemberName("dynamic-overlays")] DynamicOverlays,
    [JsonStringEnumMemberName("chroma-key")] ChromaKey,
    [JsonStringEnumMemberName("smart-framing")] SmartFraming,
    [JsonStringEnumMemberName("audio-mixer")] AudioMixer,
    [JsonStringEnumMemberName("local-audio-capture")] LocalAudioCapture,
    [JsonStringEnumMemberName("audio-monitor-output")] AudioMonitorOutput,
    [JsonStringEnumMemberName("program-recording")] ProgramRecording,
    [JsonStringEnumMemberName("iso-recording")] IsoRecording,
    [JsonStringEnumMemberName("rtmp-output")] RtmpOutput,
    [JsonStringEnumMemberName("ndi-output")] NdiOutput,
    [JsonStringEnumMemberName("srt-output")] SrtOutput,
    [JsonStringEnumMemberName("srt-ingest")] SrtIngest,
    [JsonStringEnumMemberName("webrtc-output")] WebrtcOutput,
    [JsonStringEnumMemberName("virtual-camera")] VirtualCamera,
    [JsonStringEnumMemberName("decklink-capture")] DecklinkCapture,
    [JsonStringEnumMemberName("aja-capture")] AjaCapture
}

public sealed class NativeMediaCoreProfile
{
    public required string Name { get; init; }
    public required string Renderer { get; init; }
    public required string MaxProgramResolution { get; init; }
    public int MaxProgramFps { get; init; }
    public int MaxParticipantFeeds { get; init; }
    public int MaxIsoRecordings { get; init; }
    public IReadOnlyList<string> Capabilities { get; init; } = [];
}

public sealed class NativeMediaCoreCommand
{
    [JsonPropertyName("type")]
    public required string Type { get; init; }

    [JsonExtensionData]
    public Dictionary<string, JsonElement>? ExtensionData { get; init; }
}

public sealed class MediaCoreHealth
{
    public int RestartCount { get; init; }
    public bool Recovering { get; init; }
    public bool Stopped { get; init; }

    /// <summary>
    /// Bounded, most-recent-last history of media-core child exits. Mirrors the
    /// React shell's SupportBundleCrashEvent[] (src/engine/supportBundle.ts).
    /// </summary>
    public IReadOnlyList<MediaCoreCrashEvent> CrashEvents { get; init; } = [];
}

/// <summary>
/// A single media-core child process exit captured by the supervisor. Mirrors
/// the React shell's SupportBundleCrashEvent (src/domain/production.ts).
/// </summary>
public sealed record MediaCoreCrashEvent
{
    /// <summary>ISO-8601 (UTC) timestamp of the observed child exit.</summary>
    public required string At { get; init; }

    /// <summary>Child process exit code, or null when it could not be read.</summary>
    public int? ExitCode { get; init; }

    /// <summary>Cumulative restart count after this exit was observed.</summary>
    public int RestartCount { get; init; }
}

public sealed class NativeMediaCoreColorGrade
{
    public string Lut { get; init; } = "none";
    public double Exposure { get; init; }
    public double Contrast { get; init; }
    public double Saturation { get; init; }
    public double Temperature { get; init; }
}

public sealed class NativeMediaCoreOutputProfile
{
    public required string ProfileId { get; init; }
    public required string Resolution { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int Fps { get; init; }
    public double TargetBitrateMbps { get; init; }
}

public sealed class NativeMediaCoreFrame
{
    public required string SourceId { get; init; }
    public string? ParticipantId { get; init; }
    public required string Kind { get; init; }
    public int FrameNumber { get; init; }
    public double TimestampMs { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int Fps { get; init; }
    public required string Health { get; init; }
}

public sealed class NativeMediaCoreFrameSourceSnapshot
{
    public required string AdapterId { get; init; }
    public required string Kind { get; init; }
    public required string Status { get; init; }
    public int SubscribedSourceCount { get; init; }
    public int LiveFrameCount { get; init; }
    public int StaleFrameCount { get; init; }
    public int DroppedFrameCount { get; init; }
    public int LowResolutionFrameCount { get; init; }
    public double? LastFrameTimestampMs { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed record NativeMediaCoreProgramFramePreview
{
    public int FrameNumber { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public string? RenderPlanId { get; init; }
    public string Renderer { get; init; } = "software";
    public string Health { get; init; } = "live";
    public string PixelFormat { get; init; } = "bgra";
    public byte[]? Bgra { get; init; }
    public string? BgraBase64 { get; init; }
    public NativeMediaCoreProgramSharedTexture? SharedTexture { get; init; }
}

public sealed record NativeMediaCoreProgramSharedTexture
{
    public string SharedHandleHex { get; init; } = string.Empty;
    public int Width { get; init; }
    public int Height { get; init; }
    public string Format { get; init; } = "B8G8R8A8_UNORM";
    public int FrameNumber { get; init; }
}

public sealed record NativeMediaCoreRenderedVideoSource
{
    public string LayerId { get; init; } = string.Empty;
    public string SourceId { get; init; } = string.Empty;
    public string ParticipantId { get; init; } = string.Empty;
    public string Kind { get; init; } = string.Empty;
}

public sealed record NativeMediaCoreProgramFrame
{
    public int FrameNumber { get; init; }
    public double TimestampMs { get; init; }
    public required string RenderPlanId { get; init; }
    public string? SceneId { get; init; }
    public IReadOnlyList<NativeMediaCoreRenderedVideoSource>? VideoSources { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int Fps { get; init; }
    public int LayerCount { get; init; }
    public NativeMediaCoreColorGrade ColorGrade { get; init; } = new();
    public required string Health { get; init; }
    public string? Warning { get; init; }
}

public sealed record NativeMediaCoreProgramFrameTransport
{
    public string TransportId { get; init; } = "in-process-preview";
    public required string Status { get; init; }
    public int? FrameNumber { get; init; }
    public string? RenderPlanId { get; init; }
    public double? TimestampMs { get; init; }
    public double LatencyMs { get; init; }
    public string? Warning { get; init; }
}

public sealed record NativeMediaCoreCompositorState
{
    public required string Status { get; init; }
    public string? RenderPlanId { get; init; }
    public int ProgramFrameCount { get; init; }
    public int DroppedFrameCount { get; init; }
    public int DegradedFrameCount { get; init; }
    public string? LastReconfigureReason { get; init; }
    public NativeMediaCoreProgramFrame? LastFrame { get; init; }
}

public sealed class NativeMediaCoreResolvedRoute
{
    public required string RouteId { get; init; }
    public required string Mode { get; init; }
    public required string AudioRole { get; init; }
    public string? SourceId { get; init; }
    public string? ParticipantId { get; init; }
    public string? Kind { get; init; }
    public required string Status { get; init; }
    public string? FitMode { get; init; }
    public double? SourceScale { get; init; }
    public double? SourceOffsetX { get; init; }
    public double? SourceOffsetY { get; init; }
    public NativeMediaCorePtz? Ptz { get; init; }
    public string? Warning { get; init; }
}

public sealed class NativeMediaCoreRenderPlanLayer
{
    public required string LayerId { get; init; }
    public required string Kind { get; init; }
    public string? SourceId { get; init; }
    public string? ParticipantId { get; init; }
    public string? OverlayId { get; init; }
    public int Order { get; init; }
    public string? RouteId { get; init; }
    public string? FitMode { get; init; }
    public double? SourceScale { get; init; }
    public double? SourceOffsetX { get; init; }
    public double? SourceOffsetY { get; init; }
    public NativeMediaCorePtz? Ptz { get; init; }
    public string? Position { get; init; }
}

public sealed class NativeMediaCorePtz
{
    public double Zoom { get; init; } = 1;
    public double Pan { get; init; }
    public double Tilt { get; init; }
}

public sealed class NativeMediaCoreRenderPlan
{
    public required string RenderPlanId { get; init; }
    public string? SceneId { get; init; }
    public NativeMediaCoreOutputProfile OutputProfile { get; init; } = new()
    {
        ProfileId = "1080p60",
        Resolution = "1920x1080"
    };
    public NativeMediaCoreColorGrade ColorGrade { get; init; } = new();
    public int SourceCount { get; init; }
    public int ResolvedRouteCount { get; init; }
    public IReadOnlyList<NativeMediaCoreRenderPlanLayer> Layers { get; init; } = [];
    public IReadOnlyList<NativeMediaCoreResolvedRoute> Routes { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreEncoderLifecycle
{
    public required string Status { get; init; }
    public double? PreparedAtMs { get; init; }
    public double? StartedAtMs { get; init; }
    public double? StoppedAtMs { get; init; }
    public required string LastTransition { get; init; }
}

public sealed class NativeMediaCoreEncoderTarget
{
    public required string TargetId { get; init; }
    public required string Destination { get; init; }
    public required string StreamKind { get; init; }
    public string? ParticipantId { get; init; }
    public required string Status { get; init; }
    public int AttachedFrameCount { get; init; }
    public string? Warning { get; init; }
}

public sealed class NativeMediaCoreEncoderSession
{
    public required string Status { get; init; }
    public string? RenderPlanId { get; init; }
    public int ProgramFrameCount { get; init; }
    public long EncoderQueueDroppedVideoFrames { get; init; }
    public long EncoderQueueDroppedAudioPackets { get; init; }
    public IReadOnlyList<NativeMediaCoreEncoderTarget> Targets { get; init; } = [];
    public NativeMediaCoreEncoderLifecycle Lifecycle { get; init; } = new() { Status = "idle", LastTransition = "idle" };
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreOutputHealth
{
    public required string Destination { get; init; }
    public required string Status { get; init; }
    public required string Message { get; init; }
    public int DroppedFrames { get; init; }
}

public sealed class NativeMediaCoreOutputSender
{
    public required string SenderId { get; init; }
    public required string Destination { get; init; }
    public required string Status { get; init; }
    public double? StartedAtMs { get; init; }
    public double? StoppedAtMs { get; init; }
    public int? LastFrameNumber { get; init; }
    public int FramesSent { get; init; }
    public int RetryCount { get; init; }
    public double LatencyMs { get; init; }
    public double BitrateMbps { get; init; }
    public long AudioFramesSent { get; init; }
    public long AudioBytesSent { get; init; }
    public int AudioChannels { get; init; }
    public int AudioSampleRate { get; init; }
    public string? Warning { get; init; }
    public string? LastError { get; init; }
    public string? LastResultCode { get; init; }
    public string? RuntimeDetail { get; init; }
}

public sealed class NativeMediaCoreOutputSenderSession
{
    public required string Status { get; init; }
    public int ActiveSenderCount { get; init; }
    public IReadOnlyList<NativeMediaCoreOutputSender> Senders { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

// Read model for the virtual-camera snapshot block (virtual-camera-spec V2/V4).
// Mirrors MediaCore::virtualCameraState(). status: off | starting | live | failed.
public sealed class NativeMediaCoreVirtualCameraResolution
{
    public int Width { get; init; }
    public int Height { get; init; }
}

public sealed class NativeMediaCoreVirtualCamera
{
    public bool Enabled { get; init; }
    public string Status { get; init; } = "off";
    public string DeviceName { get; init; } = "CoreVideo Pro Camera";
    public NativeMediaCoreVirtualCameraResolution Resolution { get; init; } = new();
    public int Fps { get; init; }
    public long FramesPublished { get; init; }
    public string Warning { get; init; } = string.Empty;
}

public sealed class NativeMediaCoreRecordingStream
{
    public required string Kind { get; init; }
    /// <summary>Canonical ISO source id (`zoom:&lt;pid&gt;` / `capture:&lt;id&gt;`); ISO streams only.</summary>
    public string? SourceId { get; init; }
    public string? ParticipantId { get; init; }
    /// <summary>Sanitized roster/display name used for the on-disk ISO file (ISO streams).</summary>
    public string? DisplayName { get; init; }
    public required string Path { get; init; }
    public required string Status { get; init; }
    public int FramesWritten { get; init; }
    /// <summary>Per-source audio sample-frames muxed into this ISO's raw-stem AAC track (ISO-2).</summary>
    public long AudioSamples { get; init; }
    public int DroppedFrames { get; init; }
    public long BytesWritten { get; init; }
    public string? EncoderPath { get; init; }
    public string? FallbackReason { get; init; }
    /// <summary>True when this ISO carries an audio track (a paired capture mic / Zoom stem).</summary>
    public bool HasAudio { get; init; }
    public string? Warning { get; init; }
}

public sealed record NativeMediaCoreRecordingSession
{
    public CoreVideoPro.MediaCore.Contracts.OutputLifecycle? Lifecycle { get; init; }
    public required string SessionId { get; init; }
    public bool Active { get; init; }
    public required string Status { get; init; }
    public required string WriterStatus { get; init; }
    public double StartedAtMs { get; init; }
    public double? StoppedAtMs { get; init; }
    public double ElapsedMs { get; init; }
    public required string TargetFolder { get; init; }
    public required string FilenamePrefix { get; init; }
    public required string Format { get; init; }
    public required string Quality { get; init; }
    public NativeMediaCoreRecordingEncoder Encoder { get; init; } = new();
    public double EstimatedDiskRateMBps { get; init; }
    public required string ProgramPath { get; init; }
    public IReadOnlyList<NativeMediaCoreRecordingStream> Streams { get; init; } = [];
    public NativeMediaCoreRecordingProof? Proof { get; init; }
    public int TotalFramesWritten { get; init; }
    public int TotalDroppedFrames { get; init; }
    public long TotalBytesWritten { get; init; }
    public string? Warning { get; init; }
    public string? Error { get; init; }
}

public sealed class NativeMediaCoreRecordingProof
{
    public double DurationMs { get; init; }
    public int ProgramFrameCount { get; init; }
    public int IsoFrameCount { get; init; }
    public long AudioPacketsObserved { get; init; }
    public bool AudioPresent { get; init; }
    public long AudioSampleCount { get; init; }
    public int AudioChannels { get; init; }
    public int AudioSampleRate { get; init; }
    public long EncoderQueueDroppedVideoFrames { get; init; }
    public long EncoderQueueDroppedAudioPackets { get; init; }
    public bool MetadataValid { get; init; }
    public string? ContainerFormat { get; init; }
    public string? VideoCodec { get; init; }
    public string? AudioCodec { get; init; }
    public int AudioBitrateKbps { get; init; }
    public double TargetBitrateMbps { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int FrameRate { get; init; }
    public int FailureCount { get; init; }
    public int RecoveryCount { get; init; }
}

public sealed class NativeMediaCoreRecordingEncoder
{
    public string Codec { get; init; } = "h264";
    public bool HardwareAccelerated { get; init; }
    public double TargetBitrateMbps { get; init; }
}

public sealed class NativeMediaCoreParticipantAudioChannel
{
    public required string ParticipantId { get; init; }
    public int InputLevel { get; init; }
    public int OutputLevel { get; init; }
    public double GainDb { get; init; }
    public double RmsDbfs { get; init; } = -60;
    public double PeakDbfs { get; init; } = -60;
    // C7b: live compressor gain reduction in dB (0 = idle/not engaged).
    public double GainReductionDb { get; init; }
    public double? ManualGainDb { get; init; }
    public double? Pan { get; init; }
    public bool Solo { get; init; }
    public IReadOnlyList<NativeMediaCoreAudioPluginInsert> PluginInserts { get; init; } = [];
    public bool NoiseSuppression { get; init; }
    public bool LimiterActive { get; init; }
    public bool Muted { get; init; }
    public required string Status { get; init; }
}

public sealed class NativeMediaCoreAudioPluginInsert
{
    public required string Name { get; init; }
    public required string Format { get; init; }
    public required string Status { get; init; }
    public bool ProcessingEnabled { get; init; }
}

// VST host P1 (docs/vst-host-spec.md): out-of-process plugin discovery state.
// System.Text.Json camelCase auto-binds pluginHost/plugins from the snapshot.
public sealed class NativeMediaCorePluginHost
{
    public string Status { get; init; } = "absent";
    public IReadOnlyList<NativeMediaCorePluginInfo> Plugins { get; init; } = [];
    public NativeMediaCorePluginHostServe Serve { get; init; } = new();
}

public sealed class NativeMediaCorePluginHostServe
{
    public bool Running { get; init; }
    public long Exchanges { get; init; }
    public long DeadlineMisses { get; init; }
    public string ActivePlugin { get; init; } = string.Empty;
    public int StatusCode { get; init; }
    public string LastError { get; init; } = string.Empty;
    public int EditorStatusCode { get; init; }
    public string EditorActivePlugin { get; init; } = string.Empty;
    public string EditorLastError { get; init; } = string.Empty;
    // A3: the active plugin's reported latency (0 = none). Feeds the
    // per-insert latency badge; the core compensates the mix + recording PTS.
    public long LatencySamples { get; init; }
    public double LatencyMs { get; init; }
    // A2: generic parameter surface for the ACTIVE selection. Params carries
    // the first 64 by controller index; ParamTotalCount is the plugin's real
    // total ("64 of 511 shown"). ParamValuesGeneration moves whenever any
    // published value changed (editor knob turns) — the shell's state-capture
    // debounce keys off it.
    public string ParamPluginClass { get; init; } = string.Empty;
    public int ParamTotalCount { get; init; }
    public long ParamListGeneration { get; init; }
    public long ParamValuesGeneration { get; init; }
    public IReadOnlyList<NativeMediaCoreVstParam> Params { get; init; } = [];
    // A1: serve respawn backoff telemetry. GaveUp means the isolated host
    // crashed repeatedly and the insert stays auto-bypassed (audio flows
    // unprocessed) until the operator re-selects the plug-in or reopens its
    // controls.
    public NativeMediaCorePluginHostRespawn Respawn { get; init; } = new();
}

public sealed class NativeMediaCoreVstParam
{
    public long Id { get; init; }
    public string Title { get; init; } = string.Empty;
    public string Units { get; init; } = string.Empty;
    public string Display { get; init; } = string.Empty;
    public int StepCount { get; init; }
    public double Normalized { get; init; }
}

public sealed class NativeMediaCorePluginHostRespawn
{
    public int Attempts { get; init; }
    public bool GaveUp { get; init; }
}

public sealed class NativeMediaCorePluginInfo
{
    public string Id { get; init; } = string.Empty;
    public string Name { get; init; } = string.Empty;
    public string Vendor { get; init; } = string.Empty;
    public string Probe { get; init; } = "pending";
    public IReadOnlyList<string> ClassNames { get; init; } = [];

    /// <summary>Probe verdict in operator words (U1a). Core emits pending|pass|fail.</summary>
    public string ProbeLabel => Probe switch
    {
        "pass" => "Ready",
        "fail" => "Failed validation",
        _ => "Not validated yet",
    };
}

/// <summary>
/// BS.1770 loudness + true-peak of the POST-mastering master bus (the core
/// meters the routed master AFTER processMasteringChain ran — B2 rack meters).
/// Values are LUFS/dBTP; -120 = meter not primed yet.
/// </summary>
public sealed class NativeMediaCoreMasterMeter
{
    public double MomentaryLufs { get; init; } = -120.0;
    public double ShortTermLufs { get; init; } = -120.0;
    public double IntegratedLufs { get; init; } = -120.0;
    public double TruePeakDbfs { get; init; } = -120.0;
    public int WindowMs { get; init; }
}

public sealed class NativeMediaCoreAudioMixSession
{
    public required string Status { get; init; }
    public int MasterLevel { get; init; }
    public double LoudnessLufs { get; init; }
    public NativeMediaCoreMasterMeter MasterMeter { get; init; } = new();
    public NativeMediaCorePluginHost PluginHost { get; init; } = new();
    public bool LimiterEnabled { get; init; } = true;
    public bool LimiterActive { get; init; }
    public bool MasteringEnabled { get; init; }
    public double MasteringRideDb { get; init; }
    public int MixedFrameCount { get; init; }
    public bool MonitorEnabled { get; init; }
    public string? MonitorStatus { get; init; }
    public string? MonitorDeviceId { get; init; }
    public string? MonitorDeviceName { get; init; }
    public double MonitorVolume { get; init; }
    public int MonitorFramesPlayed { get; init; }
    public IReadOnlyList<NativeMediaCoreParticipantAudioChannel> Participants { get; init; } = [];
    public required string Summary { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreCaptionCue
{
    public required string Text { get; init; }
    public string? Speaker { get; init; }
    public double AtMs { get; init; }
    public double Confidence { get; init; }
}

public sealed class NativeMediaCoreCaptionTrack
{
    public bool Enabled { get; init; }
    public required string Status { get; init; }
    public NativeMediaCoreCaptionCue? CurrentCue { get; init; }
    public double LatencyMs { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreBrandKit
{
    public required string Name { get; init; }
    public required string LogoText { get; init; }
    public string? LogoAssetId { get; init; }
    public string? LogoAssetName { get; init; }
    public string? LogoAssetPath { get; init; }
    public required string BrandColor { get; init; }
    public required string AccentColor { get; init; }
    public required string BackgroundColor { get; init; }
    public required string FontFamily { get; init; }
    public required string LowerThirdStyle { get; init; }
    public required string CaptionStyle { get; init; }
    public required string DefaultOverlayBehavior { get; init; }
    public int AppliedOverlayCount { get; init; }
    public required string Summary { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreOverlayAssetState
{
    public required string OverlayId { get; init; }
    public required string Kind { get; init; }
    public required string Position { get; init; }
    public string? SourceId { get; init; }
    public string? SourceName { get; init; }
    public string? Title { get; init; }
    public string? Org { get; init; }
    public string? Text { get; init; }
    public required string KeyPosition { get; init; }
    public required string KeyPhase { get; init; }
    public double KeyProgress { get; init; }
    public required string Keyer { get; init; }
    public int BuildInMs { get; init; }
    public int BuildOutMs { get; init; }
    public bool Visible { get; init; }
}

public sealed class NativeMediaCoreOverlayState
{
    public required string Status { get; init; }
    public int OverlayCount { get; init; }
    public int LowerThirdCount { get; init; }
    public int OnAirCount { get; init; }
    public int BuildingCount { get; init; }
    public int HiddenCount { get; init; }
    public IReadOnlyList<NativeMediaCoreOverlayAssetState> Overlays { get; init; } = [];
    public required string Summary { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreMediaPlaybackState
{
    public required string Status { get; init; }
    public string? MediaAssetId { get; init; }
    public string? MediaAssetName { get; init; }
    public string? MediaAssetKind { get; init; }
    public string? MediaAssetPath { get; init; }
    public string? MediaPlaybackKey { get; init; }
    public bool Playing { get; init; }
    public required string Summary { get; init; }
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public sealed class NativeMediaCoreOperatorAction
{
    public required string ActionId { get; init; }
    public required string Severity { get; init; }
    public required string Area { get; init; }
    public required string Title { get; init; }
    public required string Detail { get; init; }
    public string? Command { get; init; }
    public string? RelatedId { get; init; }
}

public sealed class NativeMediaCoreEvent
{
    public required string EventId { get; init; }
    public double AtMs { get; init; }
    public required string Severity { get; init; }
    public required string Area { get; init; }
    public required string Title { get; init; }
    public required string Detail { get; init; }
    public string? RelatedId { get; init; }
    public string? CommandType { get; init; }
}

public sealed record NativeMediaCoreDiagnosticsSnapshot
{
    public double GeneratedAtMs { get; init; }
    public string? SceneId { get; init; }
    public int RouteCount { get; init; }
    public int FrameCount { get; init; }
    public int ProgramFrameCount { get; init; }
    public IReadOnlyList<string> Outputs { get; init; } = [];
    public NativeMediaCoreOutputProfile OutputProfile { get; init; } = new()
    {
        ProfileId = "1080p60",
        Resolution = "1920x1080"
    };
    public IReadOnlyList<NativeMediaCoreOutputHealth> OutputHealth { get; init; } = [];
    public NativeMediaCoreOutputSenderSession OutputSenderSession { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreFrameSourceSnapshot SourceSnapshot { get; init; } = new()
    {
        AdapterId = "idle",
        Kind = "zoom-sdk",
        Status = "idle"
    };
    public NativeMediaCoreRenderPlan RenderPlan { get; init; } = new() { RenderPlanId = "idle" };
    public NativeMediaCoreCompositorState Compositor { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreProgramFrame? ProgramFrame { get; init; }
    public NativeMediaCoreProgramFramePreview? ProgramFramePreview { get; init; }
    public NativeMediaCoreProgramFrameTransport ProgramTransport { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreEncoderSession EncoderSession { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreRecordingSession? Recording { get; init; }
    public NativeMediaCoreAudioMixSession AudioMixSession { get; init; } = new() { Status = "idle", Summary = "Idle" };
    public NativeMediaCoreAudioRoutingMatrix AudioRoutingMatrix { get; init; } = new()
    {
        Status = "idle",
        Summary = "Audio routing matrix idle."
    };
    public NativeMediaCoreCaptureAudioSources CaptureAudioSources { get; init; } = new()
    {
        Status = "idle",
        Summary = "Capture audio source pairing idle."
    };
    public NativeMediaCoreCaptionTrack CaptionTrack { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreBrandKit BrandKit { get; init; } = new()
    {
        Name = "Default",
        LogoText = "CV",
        BrandColor = "#111111",
        AccentColor = "#3366ff",
        BackgroundColor = "#000000",
        FontFamily = "Inter",
        LowerThirdStyle = "solid",
        CaptionStyle = "medium sentence captions",
        DefaultOverlayBehavior = "all-off",
        Summary = "Idle"
    };
    public NativeMediaCoreOverlayState OverlayState { get; init; } = new()
    {
        Status = "idle",
        Summary = "No overlays."
    };
    public NativeMediaCoreMediaPlaybackState MediaPlayback { get; init; } = new()
    {
        Status = "idle",
        Summary = "No media asset selected."
    };
    public IReadOnlyList<NativeMediaCoreOperatorAction> OperatorActions { get; init; } = [];
    public IReadOnlyList<NativeMediaCoreEvent> EventLog { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
    public IReadOnlyList<string> LastCommandTypes { get; init; } = [];
}

/// <summary>
/// Deterministic on-device AI director recommendation, surfaced by the native
/// core in the snapshot. Mirrors the C++ Director kernel (native/src/core/Director.h)
/// and the TS protocol type MediaCoreAutoProduction.
/// </summary>
public sealed record NativeMediaCoreAutoProduction
{
    public string RuleId { get; init; } = string.Empty;
    public string RecommendedSceneId { get; init; } = string.Empty;
    public int Confidence { get; init; }
    public string Rationale { get; init; } = string.Empty;
}

public sealed record NativeMediaCorePreviewScene
{
    public string? SceneId { get; init; }
    public int RouteCount { get; init; }
    public int LayerCount { get; init; }
    public bool Composite { get; init; }
}

public sealed record NativeMediaCoreStateSnapshot
{
    public NativeMediaCorePreviewScene? PreviewScene { get; init; }
    public string? SceneId { get; init; }
    public int RouteCount { get; init; }
    public int FrameCount { get; init; }
    public IReadOnlyList<NativeMediaCoreFrame> Frames { get; init; } = [];
    public NativeMediaCoreFrameSourceSnapshot SourceSnapshot { get; init; } = new()
    {
        AdapterId = "idle",
        Kind = "zoom-sdk",
        Status = "idle"
    };
    public NativeMediaCoreProgramFrame? ProgramFrame { get; init; }
    public NativeMediaCoreProgramFramePreview? ProgramFramePreview { get; init; }
    public int ProgramFrameCount { get; init; }
    public NativeMediaCoreProgramFrameTransport ProgramTransport { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreCompositorState Compositor { get; init; } = new() { Status = "idle" };
    public int ParticipantTransformCount { get; init; }
    public int OverlayCount { get; init; }
    public IReadOnlyList<string> Outputs { get; init; } = [];
    public IReadOnlyList<string> IsoParticipantIds { get; init; } = [];
    public NativeMediaCoreOutputProfile OutputProfile { get; init; } = new()
    {
        ProfileId = "1080p60",
        Resolution = "1920x1080"
    };
    public IReadOnlyList<NativeMediaCoreOutputHealth> OutputHealth { get; init; } = [];
    public NativeMediaCoreOutputSenderSession OutputSenderSession { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreVirtualCamera VirtualCamera { get; init; } = new();
    public int SourceCount { get; init; }
    public int ResolvedRouteCount { get; init; }
    public NativeMediaCoreRenderPlan RenderPlan { get; init; } = new() { RenderPlanId = "idle" };
    public NativeMediaCoreEncoderSession EncoderSession { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreRecordingSession? Recording { get; init; }
    public NativeMediaCoreAudioMixSession AudioMixSession { get; init; } = new() { Status = "idle", Summary = "Idle" };
    public NativeMediaCoreAudioRoutingMatrix AudioRoutingMatrix { get; init; } = new()
    {
        Status = "idle",
        Summary = "Audio routing matrix idle."
    };
    public NativeMediaCoreCaptureAudioSources CaptureAudioSources { get; init; } = new()
    {
        Status = "idle",
        Summary = "Capture audio source pairing idle."
    };
    public NativeMediaCoreCaptionTrack CaptionTrack { get; init; } = new() { Status = "idle" };
    public NativeMediaCoreBrandKit BrandKit { get; init; } = new()
    {
        Name = "Default",
        LogoText = "CV",
        BrandColor = "#111111",
        AccentColor = "#3366ff",
        BackgroundColor = "#000000",
        FontFamily = "Inter",
        LowerThirdStyle = "solid",
        CaptionStyle = "medium sentence captions",
        DefaultOverlayBehavior = "all-off",
        Summary = "Idle"
    };
    public NativeMediaCoreOverlayState OverlayState { get; init; } = new()
    {
        Status = "idle",
        Summary = "No overlays."
    };
    public NativeMediaCoreMediaPlaybackState MediaPlayback { get; init; } = new()
    {
        Status = "idle",
        Summary = "No media asset selected."
    };
    public IReadOnlyList<NativeMediaCoreOperatorAction> OperatorActions { get; init; } = [];
    public IReadOnlyList<NativeMediaCoreEvent> EventLog { get; init; } = [];
    public NativeMediaCoreDiagnosticsSnapshot Diagnostics { get; init; } = new();
    public IReadOnlyList<string> LastCommandTypes { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
    /// <summary>Deterministic on-device AI director scene recommendation from the native core.</summary>
    public NativeMediaCoreAutoProduction? AutoProduction { get; init; }
    /// <summary>Optional wire field: Zoom meeting state (e.g. in_meeting, idle). Stub until native core publishes it.</summary>
    public string? MeetingState { get; init; }
    /// <summary>Optional wire field: active breakout room id. Stub until native core publishes it.</summary>
    public string? BreakoutRoomId { get; init; }
    /// <summary>Optional wire field: active breakout room label.</summary>
    public string? BreakoutRoomName { get; init; }
    /// <summary>Live Zoom roster from media-core sync when the engine is connected.</summary>
    public string? ActiveSpeakerId { get; init; }
    public IReadOnlyList<RawParticipantEvent> Participants { get; init; } = [];
    /// <summary>Latest per-source Zoom SDK subscription evidence, retained for
    /// operator-facing source diagnostics instead of being collapsed into only
    /// aggregate frame counters.</summary>
    public IReadOnlyList<ZoomMediaSpineSubscription> ZoomSubscriptions { get; init; } = [];
}

public sealed class NativeMediaCoreValidation
{
    public bool Ready { get; init; }
    public IReadOnlyList<string> MissingCapabilities { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
}

public static class NativeMediaCoreProfileValidator
{
    public static readonly IReadOnlyList<string> RequiredMvpCapabilities =
    [
        "zoom-raw-video",
        "zoom-raw-audio",
        "gpu-compositor",
        "scene-graph-rendering",
        "dynamic-overlays",
        "chroma-key",
        "smart-framing",
        "audio-mixer",
        "local-audio-capture",
        "audio-monitor-output",
        "program-recording",
        "iso-recording",
        "rtmp-output"
    ];

    public static NativeMediaCoreValidation Validate(NativeMediaCoreProfile profile)
    {
        var missing = RequiredMvpCapabilities
            .Where(capability => !profile.Capabilities.Contains(capability, StringComparer.Ordinal))
            .ToList();
        var warnings = new List<string>();

        if (profile.Renderer.Equals("software", StringComparison.Ordinal))
        {
            warnings.Add("Software rendering is not suitable for production 1080p/4K switching.");
        }

        if (profile.MaxParticipantFeeds < 8)
        {
            warnings.Add("MVP target expects at least 8 clean Zoom participant feeds.");
        }

        if (profile.MaxIsoRecordings < 8)
        {
            warnings.Add("MVP target expects program recording plus up to 8 selected Zoom ISO recovery paths.");
        }

        if (!profile.MaxProgramResolution.Equals("3840x2160", StringComparison.Ordinal))
        {
            warnings.Add("4K output will be unavailable on this media core profile.");
        }

        return new NativeMediaCoreValidation
        {
            Ready = missing.Count == 0 &&
                    !profile.Renderer.Equals("software", StringComparison.Ordinal) &&
                    profile.MaxParticipantFeeds >= 8 &&
                    profile.MaxIsoRecordings >= 8,
            MissingCapabilities = missing,
            Warnings = warnings
        };
    }
}
