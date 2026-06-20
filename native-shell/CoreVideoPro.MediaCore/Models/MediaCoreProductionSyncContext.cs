namespace CoreVideoPro.MediaCore.Models;

public sealed record MediaCoreSceneRouteWire(
    string RouteId,
    string Mode,
    string AudioRole,
    string? ParticipantId,
    double? RectX = null,
    double? RectY = null,
    double? RectWidth = null,
    double? RectHeight = null,
    int? ZIndex = null,
    string? CaptureDeviceId = null,
    string? FitMode = null,
    string? BorderStyle = null,
    string? BorderColor = null,
    double? BorderThickness = null,
    MediaCoreColorGradeWire? ColorGrade = null);

public sealed record MediaCoreParticipantWire(
    string Id,
    string Name,
    string Role,
    string BreakoutRoomId,
    string BreakoutRoomName,
    bool IsActiveSpeaker,
    bool IsMuted,
    bool IsScreenSharing,
    int AudioLevel,
    string Health);

public sealed record MediaCoreGraphicWire(
    string Id,
    string Text,
    string Position,
    bool Enabled);

public sealed record MediaCoreLowerThirdKeyWire(
    string SourceId,
    string SourceName,
    string Title,
    string Org,
    string Position,
    string Phase,
    bool Enabled);

public sealed record MediaCoreAudioMixChannelWire(
    string ParticipantId,
    int InputLevel,
    bool Muted,
    bool NoiseSuppression,
    double? ManualGainDb,
    double Pan = 0,
    bool Solo = false,
    IReadOnlyList<string>? PluginInserts = null);

/// <summary>One routed crosspoint in the audio routing gain matrix.</summary>
public sealed record MediaCoreAudioRoutingSendWire(
    string SourceId,
    string BusId,
    double GainDb,
    IReadOnlyList<string>? BusPluginInserts = null);

public sealed record MediaCoreCaptureAudioSourceWire(
    string CaptureDeviceId,
    string? AudioDeviceId,
    string? AudioDeviceName,
    int AudioSyncOffsetMs,
    string AudioSourceKind = "none",
    string? NativeAudioDeviceId = null,
    string? AudioDriverName = null,
    bool Embedded = false);

public sealed record MediaCoreAudioMonitorWire(
    bool Enabled,
    string? DeviceId,
    string? DeviceName,
    double Volume);

public sealed record MediaCoreColorGradeWire(
    string Lut,
    int Exposure,
    int Contrast,
    int Saturation,
    int Temperature);

public sealed record MediaCoreBrandKitWire(
    string Name,
    string LogoText,
    string? LogoAssetId,
    string? LogoAssetName,
    string? LogoAssetPath,
    string BrandColor,
    string AccentColor,
    string BackgroundColor,
    string FontFamily,
    string LowerThirdStyle,
    string CaptionStyle,
    string DefaultOverlayBehavior);

public sealed record MediaCoreRecordingTargetsWire(
    string TargetFolder,
    string FilenamePrefix,
    string Format,
    string Quality,
    IReadOnlyList<string> IsoParticipantIds);

public sealed record MediaCoreStreamDestinationWire(
    string Id,
    string Label,
    string? Protocol = null,
    string? Url = null,
    string? StreamKey = null,
    string? Mode = null,
    string? Host = null,
    int? Port = null,
    int? LatencyMs = null,
    int? LatencyUs = null,
    string? Passphrase = null,
    int? KeyLength = null,
    string? StreamId = null,
    string? NdiName = null,
    string? NdiGroup = null,
    string? FfmpegBinDirectory = null,
    int? Fps = null,
    double? TargetBitrateMbps = null,
    string? VideoCodec = null);

public sealed record MediaCoreSrtIngestSourceWire(
    string Id,
    string DeviceId,
    string Name,
    string Mode,
    string Host,
    int Port,
    int LatencyMs,
    string? StreamId = null,
    string? Passphrase = null);

public sealed record MediaCoreOutputProfileWire(
    string ProfileId,
    string Resolution,
    int Width,
    int Height,
    int Fps,
    double TargetBitrateMbps,
    string Codec = "h264");

/// <summary>
/// Production inputs for building a media-core-sync command batch.
/// Mirrors the React <c>buildNativeMediaCoreCommands</c> production state slice.
/// </summary>
public sealed record MediaCoreProductionSyncContext
{
    public required string ActiveSceneId { get; init; }
    public IReadOnlyList<MediaCoreSceneRouteWire> SceneRoutes { get; init; } = [];
    public IReadOnlyList<MediaCoreParticipantWire> Participants { get; init; } = [];
    public bool Recording { get; init; }
    public bool Streaming { get; init; }
    public IReadOnlyList<string> StreamDestinations { get; init; } = ["rtmp"];
    public IReadOnlyList<MediaCoreStreamDestinationWire> StreamDestinationSettings { get; init; } = [];
    public IReadOnlyList<MediaCoreSrtIngestSourceWire> SrtIngestSources { get; init; } = [];
    public MediaCoreOutputProfileWire CanvasOutputProfile { get; init; } = DefaultCanvasOutputProfile;
    public MediaCoreOutputProfileWire StreamOutputProfile { get; init; } = DefaultStreamOutputProfile;
    public MediaCoreOutputProfileWire RecordingOutputProfile { get; init; } = DefaultRecordingOutputProfile;
    public MediaCoreRecordingTargetsWire RecordingTargets { get; init; } = DefaultRecordingTargets;
    public IReadOnlyList<MediaCoreGraphicWire> Graphics { get; init; } = [];
    public MediaCoreLowerThirdKeyWire? LowerThirdKey { get; init; }
    public MediaCoreColorGradeWire ColorGrade { get; init; } = NeutralColorGrade;
    public MediaCoreBrandKitWire BrandKit { get; init; } = DefaultBrandKit;
    public bool AudioLimiterEnabled { get; init; } = true;
    public MediaCoreAudioMonitorWire AudioMonitor { get; init; } = new(false, null, null, 0);
    public IReadOnlyList<MediaCoreAudioMixChannelWire> AudioMixChannels { get; init; } = [];
    public IReadOnlyList<MediaCoreAudioRoutingSendWire> AudioRoutingSends { get; init; } = [];
    public IReadOnlyList<MediaCoreCaptureAudioSourceWire> CaptureAudioSources { get; init; } = [];
    public string? CaptionText { get; init; }
    public string? CaptionSpeaker { get; init; }
    public string? SelectedMediaAssetId { get; init; }
    public string? SelectedMediaAssetName { get; init; }
    public string? SelectedMediaAssetKind { get; init; }
    public string? SelectedMediaAssetPath { get; init; }
    public bool SelectedMediaAssetPlaying { get; init; }

    public static MediaCoreOutputProfileWire DefaultCanvasOutputProfile { get; } = new(
        ProfileId: "1080p60",
        Resolution: "1920x1080",
        Width: 1920,
        Height: 1080,
        Fps: 60,
        TargetBitrateMbps: 8.2);

    public static MediaCoreOutputProfileWire DefaultStreamOutputProfile { get; } = DefaultCanvasOutputProfile;

    public static MediaCoreOutputProfileWire DefaultRecordingOutputProfile { get; } = DefaultCanvasOutputProfile;

    public static MediaCoreRecordingTargetsWire DefaultRecordingTargets { get; } = new(
        TargetFolder: "Recordings/CoreVideo Pro",
        FilenamePrefix: "corevideo-recording",
        Format: "mp4",
        Quality: "high",
        IsoParticipantIds: []);

    public static MediaCoreColorGradeWire NeutralColorGrade { get; } = new(
        Lut: "none",
        Exposure: 0,
        Contrast: 0,
        Saturation: 0,
        Temperature: 0);

    public static MediaCoreBrandKitWire DefaultBrandKit { get; } = new(
        Name: "CoreVideo",
        LogoText: "CoreVideo",
        LogoAssetId: null,
        LogoAssetName: null,
        LogoAssetPath: null,
        BrandColor: "#44c1a1",
        AccentColor: "#f0a85c",
        BackgroundColor: "#0c1118",
        FontFamily: "Inter",
        LowerThirdStyle: "gradient",
        CaptionStyle: "medium sentence captions",
        DefaultOverlayBehavior: "all-off");
}
