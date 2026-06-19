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
    int? ZIndex = null);

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

public sealed record MediaCoreAudioMixChannelWire(
    string ParticipantId,
    int InputLevel,
    bool Muted,
    bool NoiseSuppression,
    double? ManualGainDb);

/// <summary>One routed crosspoint in the audio routing gain matrix.</summary>
public sealed record MediaCoreAudioRoutingSendWire(
    string SourceId,
    string BusId,
    double GainDb);

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
    public MediaCoreRecordingTargetsWire RecordingTargets { get; init; } = DefaultRecordingTargets;
    public IReadOnlyList<MediaCoreGraphicWire> Graphics { get; init; } = [];
    public MediaCoreColorGradeWire ColorGrade { get; init; } = NeutralColorGrade;
    public MediaCoreBrandKitWire BrandKit { get; init; } = DefaultBrandKit;
    public IReadOnlyList<MediaCoreAudioMixChannelWire> AudioMixChannels { get; init; } = [];
    public IReadOnlyList<MediaCoreAudioRoutingSendWire> AudioRoutingSends { get; init; } = [];
    public string? CaptionText { get; init; }
    public string? CaptionSpeaker { get; init; }
    public string? SelectedMediaAssetId { get; init; }
    public string? SelectedMediaAssetName { get; init; }
    public string? SelectedMediaAssetKind { get; init; }
    public string? SelectedMediaAssetPath { get; init; }
    public bool SelectedMediaAssetPlaying { get; init; }

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
