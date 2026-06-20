using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// Identifies which operator monitor tile a surface represents.
/// </summary>
public enum VideoSurfaceKind
{
    Program,
    Preview,
    Multiview,
    Participant
}

/// <summary>
/// Transport-neutral frame metadata mirrored from media-core events and sync snapshots.
/// RGBA pixels and GPU handles stay off the wire until shared-texture export lands.
/// </summary>
public sealed class VideoFrameMetadata
{
    public string? ParticipantId { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public int FrameId { get; init; }
    public double Fps { get; init; }
    public string Renderer { get; init; } = "pending";
    public string Health { get; init; } = "idle";
    public string? RenderPlanId { get; init; }
    public int LayerCount { get; init; }
    public long TimestampMs { get; init; }

    public string ResolutionLabel => Width > 0 && Height > 0 ? $"{Width}×{Height}" : "—";

    public string FpsLabel => Fps > 0 ? $"{Fps:0.#} fps" : "— fps";
}

/// <summary>
/// Future DXGI shared-texture handle export from the native D3D11 compositor.
/// The C# host accepts and queues handles but does not present them yet.
/// </summary>
public sealed class SharedTextureHandle
{
    public ulong NtHandle { get; init; }
    public int Width { get; init; }
    public int Height { get; init; }
    public string Format { get; init; } = "B8G8R8A8_UNORM";
    public long AdapterLuid { get; init; }
    public int FrameNumber { get; init; }

    public bool IsValid =>
        SharedTextureInteropRules.IsPresentableHandle(NtHandle) && Width > 0 && Height > 0;
}

/// <summary>
/// Immutable read model for a single video surface tile in the operator UI.
/// </summary>
public sealed record VideoSurfaceState
{
    public string SurfaceKey { get; init; } = string.Empty;
    public VideoSurfaceKind Kind { get; init; }
    public string Title { get; init; } = "Video";
    public string StatusLine { get; init; } = "Waiting for frames";
    public string DetailLine { get; init; } = "Start the engine to receive media-core frames.";
    public VideoFrameMetadata? LastFrame { get; init; }
    public SharedTextureHandle? PendingSharedHandle { get; init; }
    public string? MediaAssetPath { get; init; }
    public string? MediaAssetKind { get; init; }
    public bool MediaAssetPlaying { get; init; }
    public byte[]? PreviewBgra { get; init; }
    public int PreviewWidth { get; init; }
    public int PreviewHeight { get; init; }
    public bool HasFrames => LastFrame is { FrameId: > 0 };
    public bool HasPreviewBitmap => PreviewBgra is { Length: > 0 } && PreviewWidth > 0 && PreviewHeight > 0;
    public bool AwaitingDirect3D => PendingSharedHandle is { IsValid: true };

    public static VideoSurfaceState Waiting(VideoSurfaceKind kind, string surfaceKey, string title) =>
        new()
        {
            SurfaceKey = surfaceKey,
            Kind = kind,
            Title = title,
            StatusLine = "Waiting for video frames",
            DetailLine = "Waiting for this source to publish frames."
        };

    /// <summary>
    /// Initial state for an always-on compositor surface before the first program frame
    /// arrives. The media core is always running, so this resolves on its own within a
    /// couple hundred milliseconds rather than requiring the operator to start anything.
    /// </summary>
    public static VideoSurfaceState Slate(VideoSurfaceKind kind, string surfaceKey, string title) =>
        new()
        {
            SurfaceKey = surfaceKey,
            Kind = kind,
            Title = title,
            StatusLine = "Waiting for media engine",
            DetailLine = "Program output appears after media-core publishes compositor state."
        };

    public static VideoSurfaceState WaitingForFirstFrame(VideoSurfaceKind kind, string surfaceKey, string title) =>
        new()
        {
            SurfaceKey = surfaceKey,
            Kind = kind,
            Title = title,
            StatusLine = "Waiting for first compositor frame",
            DetailLine = "The media engine is running; the Program monitor will update on the first rendered frame."
        };

    /// <summary>
    /// State shown when the Zoom capture subscription is paused. The compositor keeps
    /// rendering a program slate, so this is informational rather than a hard "waiting".
    /// </summary>
    public static VideoSurfaceState CapturePaused(VideoSurfaceKind kind, string surfaceKey, string title) =>
        new()
        {
            SurfaceKey = surfaceKey,
            Kind = kind,
            Title = title,
            StatusLine = "Capture paused",
            DetailLine = "Showing program slate. Turn Capture on to ingest Zoom video."
        };

    public static VideoSurfaceState CaptureSourceOnline(VideoSurfaceKind kind, string surfaceKey, string title) =>
        new()
        {
            SurfaceKey = surfaceKey,
            Kind = kind,
            Title = title,
            StatusLine = "Source online",
            DetailLine = "Waiting for capture frames."
        };

    public static VideoSurfaceState MediaAssetPreview(string surfaceKey, string title, string path, string kind, bool playing) =>
        new()
        {
            SurfaceKey = surfaceKey,
            Kind = VideoSurfaceKind.Multiview,
            Title = title,
            StatusLine = playing ? "Media playing" : "Media cued",
            DetailLine = playing ? "Selected media is active in playout." : "Selected media is ready for playout.",
            MediaAssetPath = path,
            MediaAssetKind = kind,
            MediaAssetPlaying = playing
        };

    public VideoSurfaceState WithFrame(VideoFrameMetadata frame, string statusLine, string? detailLine = null) =>
        new()
        {
            SurfaceKey = SurfaceKey,
            Kind = Kind,
            Title = Title,
            StatusLine = statusLine,
            DetailLine = detailLine ?? DetailLine,
            LastFrame = frame,
            PendingSharedHandle = PendingSharedHandle,
            MediaAssetPath = MediaAssetPath,
            MediaAssetKind = MediaAssetKind,
            MediaAssetPlaying = MediaAssetPlaying,
            PreviewBgra = PreviewBgra,
            PreviewWidth = PreviewWidth,
            PreviewHeight = PreviewHeight
        };

    public VideoSurfaceState WithPreviewPixels(byte[] bgra, int width, int height) =>
        new()
        {
            SurfaceKey = SurfaceKey,
            Kind = Kind,
            Title = Title,
            StatusLine = StatusLine,
            DetailLine = DetailLine,
            LastFrame = LastFrame,
            PendingSharedHandle = PendingSharedHandle,
            MediaAssetPath = MediaAssetPath,
            MediaAssetKind = MediaAssetKind,
            MediaAssetPlaying = MediaAssetPlaying,
            PreviewBgra = bgra,
            PreviewWidth = width,
            PreviewHeight = height
        };

    public VideoSurfaceState WithSharedHandle(SharedTextureHandle handle) =>
        new()
        {
            SurfaceKey = SurfaceKey,
            Kind = Kind,
            Title = Title,
            StatusLine = StatusLine,
            DetailLine = "GPU shared texture ready.",
            LastFrame = LastFrame,
            PendingSharedHandle = handle,
            MediaAssetPath = MediaAssetPath,
            MediaAssetKind = MediaAssetKind,
            MediaAssetPlaying = MediaAssetPlaying
        };
}

/// <summary>
/// Multiview tile binding participant roster data to per-participant surface state.
/// </summary>
public sealed class ParticipantSurfaceTile
{
    public Participant Participant { get; init; } = new();

    public VideoSurfaceState Surface { get; init; } =
        VideoSurfaceState.Waiting(VideoSurfaceKind.Multiview, "empty", "No source");

    public int SourceIndex { get; init; }

    public bool IsEmpty { get; init; }

    public string SourceLabel => SourceIndex > 0 ? SourceIndex.ToString("00") : "—";

    public static ParticipantSurfaceTile EmptySlot(int slotNumber) =>
        new()
        {
            SourceIndex = slotNumber,
            IsEmpty = true,
            Participant = new Participant
            {
                Id = $"mv-empty-{slotNumber}",
                Name = $"Input {slotNumber:00}",
                Role = ParticipantRole.Guest,
                Health = FeedHealth.VideoOff
            },
            Surface = VideoSurfaceState.Waiting(
                VideoSurfaceKind.Multiview,
                $"mv-empty-{slotNumber}",
                $"Input {slotNumber:00}")
        };
}
