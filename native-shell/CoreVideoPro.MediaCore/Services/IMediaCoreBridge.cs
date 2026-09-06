using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// The DI seam over <see cref="MediaCoreBridgeService"/> introduced by PR2 of the
/// StudioViewModel strangler refactor. It is the surface the shell's ViewModels (and the
/// extracted <c>TransportCoordinator</c>) touch on the native-media-core bridge: lifecycle,
/// the last snapshot, the sync/poll round-trips, the Zoom spine-sync installer, the capture /
/// browser / VST passthroughs, and the change events. <see cref="MediaCoreBridgeService"/>
/// implements it verbatim; a test can supply a fake so the coordinator is constructible in
/// isolation (which StudioViewModel itself is not — it new()s ~10 services + launches the core).
/// Move-only: every member mirrors an existing <see cref="MediaCoreBridgeService"/> signature.
/// </summary>
public interface IMediaCoreBridge : IAsyncDisposable
{
    // --- change events (the shell wires all of these on construction) ---
    event Action<MediaCoreHealth>? HealthChanged;
    event Action<string>? StatusChanged;
    event Action<NativeMediaCoreProfile>? ProfileChanged;
    event Action<NativeMediaCoreStateSnapshot>? SnapshotChanged;
    event Action<ZoomVideoFrame>? ZoomVideoFrameReceived;
    event Action<ProgramFramePreview>? ProgramFramePreviewReceived;
    event Action<ProgramSharedTexture>? ProgramSharedTextureReceived;
    event Action<ProgramSharedTexture>? PreviewSharedTextureReceived;
    event Action<ParticipantSharedTexture>? ParticipantSharedTextureReceived;
    event Action<MultiviewSharedTexture>? MultiviewSharedTextureReceived;

    // --- state ---
    NativeMediaCoreProfile? Profile { get; }

    string ProfileSummary { get; }

    bool Running { get; }

    NativeMediaCoreStateSnapshot? LastSnapshot { get; }

    // --- lifecycle ---
    Task<NativeMediaCoreProfile?> StartAsync(CancellationToken cancellationToken = default);

    void Stop();

    // --- Zoom spine sync installer (the transport Engine toggle drives this) ---
    void ConfigureZoomSpineSync(Func<CancellationToken, Task<Dictionary<string, object?>>>? payloadFactory);

    // --- sync / poll ---
    Task<NativeMediaCoreStateSnapshot> SyncAsync(
        IReadOnlyList<NativeMediaCoreCommand> commands,
        double? elapsedMs = null,
        CancellationToken cancellationToken = default);

    Task<NativeMediaCoreStateSnapshot> PollSnapshotAsync(CancellationToken cancellationToken = default);

    Task<RawCaptureSnapshot> StopZoomCaptureAsync(CancellationToken cancellationToken = default);

    Task<RawCaptureSnapshot> GetZoomSnapshotAsync(CancellationToken cancellationToken = default);

    // --- VST passthroughs ---
    Task OpenVstEditorAsync(string selection, CancellationToken cancellationToken = default);

    Task SetVstParamAsync(string selection, long paramId, double normalized,
        CancellationToken cancellationToken = default);

    Task SetVstStateAsync(string selection, string stateBase64,
        CancellationToken cancellationToken = default);

    Task<string?> GetVstStateAsync(string selection, CancellationToken cancellationToken = default);

    // --- capture passthroughs ---
    Task SetCaptureAudioSyncOffsetAsync(string deviceId, int offsetMs,
        CancellationToken cancellationToken = default);

    Task RegisterCaptureShmAsync(string deviceId, string shmName, int width, int height,
        CancellationToken cancellationToken = default);

    Task<IReadOnlyList<NativeCaptureDeviceStatus>> ConnectNativeCaptureDeviceAsync(
        string deviceId,
        CancellationToken cancellationToken = default,
        string? outputSourceId = null);

    Task<IReadOnlyList<NativeCaptureDeviceStatus>> ListNativeCaptureDevicesAsync(
        CancellationToken cancellationToken = default);

    Task<IReadOnlyList<NativeCaptureDeviceStatus>> DisconnectNativeCaptureDeviceAsync(
        string deviceId,
        CancellationToken cancellationToken = default);

    // --- browser sources ---
    Task AddBrowserSourceAsync(string url, int width, int height, int fps,
        CancellationToken cancellationToken = default);

    Task RemoveBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default);

    Task ReloadBrowserSourceAsync(string browserId, CancellationToken cancellationToken = default);
}
