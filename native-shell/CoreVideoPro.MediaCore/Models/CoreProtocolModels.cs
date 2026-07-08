namespace CoreVideoPro.MediaCore.Models;

public sealed class ZoomVideoFrame
{
    public required string ParticipantId { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int FrameId { get; init; }
    public required byte[] Bgra { get; init; }
}

public sealed class CoreZoomVideoFrameEvent
{
    public string Type => "zoom-video-frame";
    public required ZoomVideoFrame Frame { get; init; }
}

public sealed class ProgramSharedTexture
{
    public string SharedHandleHex { get; init; } = string.Empty;
    public int Width { get; init; }
    public int Height { get; init; }
    public string Format { get; init; } = "B8G8R8A8_UNORM";
    public int FrameNumber { get; init; }

    public bool IsValid =>
        !string.IsNullOrWhiteSpace(SharedHandleHex) &&
        Width > 0 &&
        Height > 0;
}

public sealed class ProgramFramePreview
{
    public int FrameNumber { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public string? RenderPlanId { get; init; }
    public string Renderer { get; init; } = "software";
    public string Health { get; init; } = "live";
    public string PixelFormat { get; init; } = "bgra";
    public byte[] Bgra { get; init; } = [];
    public ProgramSharedTexture? SharedTexture { get; init; }
}

public sealed class CoreProgramFramePreviewEvent
{
    public string Type => "program-frame-preview";
    public required ProgramFramePreview Preview { get; init; }
}

public sealed class CoreProgramSharedTextureEvent
{
    public string Type => "program-shared-texture";
    public required ProgramSharedTexture Texture { get; init; }
}

/// <summary>
/// The single GPU shared texture carrying the whole core-composited PREVIEW scene
/// (routes + overlays + background + grade). Mirrors <c>program-shared-texture</c>;
/// presented on the dedicated preview monitor and the multiviewer PVW cell.
/// </summary>
public sealed class CorePreviewSharedTextureEvent
{
    public string Type => "preview-shared-texture";
    public required ProgramSharedTexture Texture { get; init; }
}

public sealed class ParticipantSharedTexture
{
    public string ParticipantId { get; init; } = string.Empty;
    public string SharedHandleHex { get; init; } = string.Empty;
    public int Width { get; init; }
    public int Height { get; init; }
    public string Format { get; init; } = "B8G8R8A8_UNORM";
    public int FrameNumber { get; init; }

    public bool IsValid =>
        !string.IsNullOrWhiteSpace(ParticipantId) &&
        !string.IsNullOrWhiteSpace(SharedHandleHex) &&
        Width > 0 &&
        Height > 0;
}

public sealed class CoreParticipantSharedTextureEvent
{
    public string Type => "participant-shared-texture";
    public required ParticipantSharedTexture Texture { get; init; }
}

/// <summary>
/// One tile in the core-composited multiview. The rect (x,y,w,h) is normalized [0,1] to the
/// multiview canvas; the active-speaker border is baked into the shared texture, so this is
/// only used to position the transparent click overlay and label.
/// </summary>
public sealed class MultiviewTile
{
    public string SourceId { get; init; } = string.Empty;
    public string ParticipantId { get; init; } = string.Empty;
    public int Slot { get; init; }
    public string Label { get; init; } = string.Empty;
    // Tile semantics for the user-selectable multiviewer layouts:
    //   Role  = "pgm" | "pvw" | "source" (the cell kind).
    //   Tally = "pgm" | "pvw" | "none"   (the source's live status).
    public string Role { get; init; } = "source";
    public string Tally { get; init; } = "none";
    public bool ActiveSpeaker { get; init; }
    public double X { get; init; }
    public double Y { get; init; }
    public double W { get; init; }
    public double H { get; init; }
}

/// <summary>
/// The single GPU shared texture carrying the whole composited multiview grid, plus the
/// normalized tile rects for the transparent click overlay. Mirrors <c>multiview-shared-texture</c>.
/// </summary>
public sealed class MultiviewSharedTexture
{
    public ProgramSharedTexture Texture { get; init; } = new();
    public int CanvasWidth { get; init; }
    public int CanvasHeight { get; init; }
    public IReadOnlyList<MultiviewTile> Tiles { get; init; } = [];

    public bool IsValid => Texture.IsValid;
}

public sealed class CoreMultiviewSharedTextureEvent
{
    public string Type => "multiview-shared-texture";
    public required MultiviewSharedTexture Multiview { get; init; }
}

public sealed class CoreError
{
    public required string Code { get; init; }
    public required string Message { get; init; }
}

public sealed class CoreResponseEnvelope
{
    public required string Id { get; init; }
    public bool Ok { get; init; }
    public string? Type { get; init; }
    public CoreError? Error { get; init; }
}

/// <summary>
/// One capture device reported by the native core's "capture-devices"
/// responses (list-capture-devices / connect-capture-device /
/// select-capture-input). Mirrors captureDeviceJson in
/// native/src/core/MediaCore.cpp.
/// </summary>
public sealed class NativeCaptureDeviceStatus
{
    public required string Id { get; init; }
    public string Name { get; init; } = "";
    public string Vendor { get; init; } = "";
    public string ConnectionState { get; init; } = "";
    public bool SignalPresent { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int FrameRate { get; init; }
    public string? Warning { get; init; }

    /// <summary>
    /// OS-level device identity (the Windows device-interface symbolic link for
    /// native UVC devices). Lets the shell correlate a core-enumerated device
    /// with its own WinRT enumeration even when the hashed stable ids disagree
    /// (symbolic-link casing differences). Null when the core device carries no
    /// OS identity (stub/virtual devices).
    /// </summary>
    public string? NativeDeviceId { get; init; }
}

public sealed class NativeMediaCoreWireHealth
{
    public string? Status { get; init; }
    public string? Renderer { get; init; }
    public string? ProgramFrameHealth { get; init; }
    public string? Encoder { get; init; }
    public string? Codec { get; init; }
    public bool? HardwareEncoder { get; init; }
    public string? RecordingArtifactPath { get; init; }
    public long? RecordingBytesWritten { get; init; }
    public int? EncodedFrameCount { get; init; }
    public int? FrameCount { get; init; }
    public IReadOnlyList<string>? Messages { get; init; }
}

public sealed class NativeMediaCoreCaptureAudioSource
{
    public required string CaptureDeviceId { get; init; }
    public string? SourceId { get; init; }
    public string? AudioDeviceId { get; init; }
    public string? AudioDeviceName { get; init; }
    public string? AudioSourceKind { get; init; }
    public string? NativeAudioDeviceId { get; init; }
    public string? AudioDriverName { get; init; }
    public bool Embedded { get; init; }
    public int AudioSyncOffsetMs { get; init; }
    public bool Paired { get; init; }
    public bool CaptureStreaming { get; init; }
    public long CaptureFramesReceived { get; init; }
    public long CaptureFramesRendered { get; init; }
    public long CaptureQueuedFrames { get; init; }
    public long CaptureUnderrunCount { get; init; }
    public long EmptyPacketPolls { get; init; }
    public double CaptureStartedAtMs { get; init; }
    public double CaptureLastFrameAtMs { get; init; }
    public double CaptureLastFrameAgeMs { get; init; }
    public double CaptureStoppedAtMs { get; init; }
    public int CaptureSampleRate { get; init; }
    public int CaptureChannels { get; init; }
    public double PeakDbfs { get; init; } = -120;
    public double RmsDbfs { get; init; } = -120;
    public bool SignalPresent { get; init; }
    public string? EndpointId { get; init; }
    public string? EndpointName { get; init; }
    public string? LastError { get; init; }
    public string? Warning { get; init; }
}

public sealed class NativeMediaCoreCaptureAudioSources
{
    public required string Status { get; init; }
    public int SourceCount { get; init; }
    public int PairedCount { get; init; }
    public int StreamingCount { get; init; }
    public long CaptureFramesReceived { get; init; }
    public int RoutedMasterFrames { get; init; }
    public int RoutedStreamFrames { get; init; }
    public int RoutedMonitorFrames { get; init; }
    public int FallbackMonitorFrames { get; init; }
    public long MonitorFramesPlayed { get; init; }
    public IReadOnlyList<NativeMediaCoreCaptureAudioSource> Sources { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
    public required string Summary { get; init; }
}

public sealed class NativeMediaCoreAudioBusTap
{
    public required string BusId { get; init; }
    public int Channels { get; init; }
    public int Frames { get; init; }
    public double PeakDbfs { get; init; }
    public double RmsDbfs { get; init; }
}

// One routed crosspoint as the CORE actually applies it (engine source ids).
// The core has always published these in the snapshot's audioRoutingMatrix
// "sends" array; the shell previously dropped them, leaving the routing grid
// a client-side ghost that never showed engine defaults or overrides (audio
// tab redesign phase B1).
public sealed class NativeMediaCoreAudioRoutingSend
{
    public required string SourceId { get; init; }
    public required string BusId { get; init; }
    public double GainDb { get; init; }
}

public sealed class NativeMediaCoreAudioRoutingMatrix
{
    public required string Status { get; init; }
    public int RoutedSendCount { get; init; }
    public int RoutedSourceCount { get; init; }
    public int ProgramTapFrames { get; init; }
    public IReadOnlyList<NativeMediaCoreAudioRoutingSend> Sends { get; init; } = [];
    public IReadOnlyList<NativeMediaCoreAudioBusTap> BusTaps { get; init; } = [];
    public IReadOnlyList<string> Warnings { get; init; } = [];
    public required string Summary { get; init; }
}

public sealed class RawCaptureSnapshot
{
    public required string MeetingState { get; init; }
    public IReadOnlyList<RawParticipantEvent> Participants { get; init; } = [];
    public string? ActiveSpeakerId { get; init; }
    public string? Caption { get; init; }
    public int Tick { get; init; }
    public IReadOnlyList<string>? Warnings { get; init; }
}

public sealed class RawParticipantEvent
{
    public required string UserId { get; init; }
    public required string DisplayName { get; init; }
    public string? Role { get; init; }
    public string? Title { get; init; }
    public string? BreakoutRoomId { get; init; }
    public string? BreakoutRoomName { get; init; }
    public bool? Muted { get; init; }
    public bool? VideoOn { get; init; }
    public bool? Talking { get; init; }
    public bool? SharingScreen { get; init; }
    public int? AudioLevel { get; init; }
    public string? NetworkQuality { get; init; }
}

public sealed class NativeMediaCoreWireState
{
    public string? SceneId { get; init; }
    public int? RouteCount { get; init; }
    public int? TransformCount { get; init; }
    public int? OverlayCount { get; init; }
    public IReadOnlyList<string>? Outputs { get; init; }
    public IReadOnlyList<string>? IsoParticipantIds { get; init; }
    public int? ProgramFrameCount { get; init; }
    public string? RenderPlanId { get; init; }
    public string? CompositorRenderer { get; init; }
    public NativeMediaCoreEncoderSession? EncoderSession { get; init; }
    public NativeMediaCoreOutputSenderSession? OutputSenderSession { get; init; }
    public NativeMediaCoreVirtualCamera? VirtualCamera { get; init; }
    public NativeMediaCoreRecordingSession? Recording { get; init; }
    public NativeMediaCoreWireHealth? Health { get; init; }
    public NativeMediaCoreProfile? Profile { get; init; }
    public NativeMediaCoreAudioMixSession? AudioMixSession { get; init; }
    public NativeMediaCoreAudioRoutingMatrix? AudioRoutingMatrix { get; init; }
    public NativeMediaCoreCaptureAudioSources? CaptureAudioSources { get; init; }
    public NativeMediaCoreCaptionTrack? CaptionTrack { get; init; }
    public NativeMediaCoreBrandKit? BrandKit { get; init; }
    public NativeMediaCoreOverlayState? OverlayState { get; init; }
    public NativeMediaCoreMediaPlaybackState? MediaPlayback { get; init; }
    public NativeMediaCoreProgramFramePreview? ProgramFramePreview { get; init; }
    public NativeMediaCoreProgramSharedTexture? ProgramSharedTexture { get; init; }
    public string? MeetingState { get; init; }
    public string? BreakoutRoomId { get; init; }
    public string? BreakoutRoomName { get; init; }
    public string? ActiveSpeakerId { get; init; }
    public IReadOnlyList<RawParticipantEvent>? Participants { get; init; }
}
