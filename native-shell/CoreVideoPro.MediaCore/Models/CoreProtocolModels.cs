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
    public long EmptyPacketPolls { get; init; }
    public int CaptureSampleRate { get; init; }
    public int CaptureChannels { get; init; }
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

public sealed class NativeMediaCoreAudioRoutingMatrix
{
    public required string Status { get; init; }
    public int RoutedSendCount { get; init; }
    public int RoutedSourceCount { get; init; }
    public int ProgramTapFrames { get; init; }
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
