using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.Windows.AppLifecycle;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel : ObservableObject, IAsyncDisposable
{
    private const string MultiviewSoloSceneAId = "multiview-solo-a";
    private const string MultiviewSoloSceneBId = "multiview-solo-b";
    private const string ManualOneUpLayout = "full";
    private const int MaxSrtIngestSources = 8;
    private const double DefaultAudioMonitorVolume = 0.75;

    private readonly MediaCoreBridgeService _bridge = new();
    private readonly MediaBinService _mediaBinService = new();
    private readonly CaptureDeviceDiscoveryService _captureDiscovery = new();
    private readonly AudioCaptureDeviceDiscoveryService _audioCaptureDiscovery = new();
    private readonly AudioRenderDeviceDiscoveryService _audioRenderDiscovery = new();
    private readonly CaptureDeviceFrameReaderService _captureFrameReader = new();
    private readonly VideoSurfaceCoordinator _surfaces = new();
    private readonly DispatcherQueue _dispatcher = DispatcherQueue.GetForCurrentThread();
    private readonly DispatcherQueueTimer _automationTimer;
    private readonly ZoomOAuthService _zoomOAuth;
    private readonly ZoomOAuthAppCoordinator _zoomOAuthCoordinator;
    private readonly string _currentRoomId;
    private readonly string _currentRoomName;
    private AudioMixerWindow? _audioMixerWindow;
    private MultiviewPopoutWindow? _multiviewPopoutWindow;
    private ProgramPreviewPopoutWindow? _programPreviewPopoutWindow;
    private ProductionSettingsWindow? _productionSettingsWindow;
    private DiagnosticsWindow? _diagnosticsWindow;
    private CancellationTokenSource? _lowerThirdKeyTransitionCts;
    // The source the lower third is currently showing OR animating toward. Compared in
    // UpdateProgramLowerThirdKey so a refresh for the same target never restarts the slide
    // (the displayed key lags behind during a build-out). Empty when hidden.
    private string _lowerThirdTargetSourceId = string.Empty;
    private long _lowerThirdKeyChangeTickMs;
    private readonly HashSet<ColorGradeEditorViewModel> _openColorGradeEditors = [];
    private IReadOnlyList<AudioCaptureDevice> _lastDiscoveredAudioCaptureDevices = [];
    private DateTimeOffset _lastAudioTelemetryLoggedAt = DateTimeOffset.MinValue;
    private string? _lastAudioTelemetrySignature;
    private readonly Dictionary<string, List<string>> _audioProcessingInserts = new(StringComparer.Ordinal);
    private readonly IProductionOutputPreferencesStore _outputPreferencesStore = CreateProductionOutputPreferencesStore();
    private bool _outputPreferencesLoaded;
    private bool _programLowerThirdAutomationSuppressed;
    private bool _applyingStreamingProfile;
    private bool _suppressMasteringSync;
    private string _masteringCompareSlot = "A";
    private MasteringSettings _masteringCompareA = MasteringPresetCatalog.Neutral;
    private MasteringSettings _masteringCompareB = MasteringPresetCatalog.Neutral;

    [ObservableProperty]
    private bool _zoomCaptureSubscribed;

    [ObservableProperty]
    private string _engineStatus = "Join a Zoom meeting to request capture.";

    [ObservableProperty]
    private string _zoomStatus = "Zoom Offline";

    [ObservableProperty]
    private StudioTab _activeTab = StudioTab.Studio;

    [ObservableProperty]
    private StudioViewMode _viewMode = StudioViewMode.ProgramPreview;

    [ObservableProperty]
    private string _activeSceneId = "speaker-slides";

    [ObservableProperty]
    private string _previewSceneId = "speaker-slides";

    [ObservableProperty]
    private string _sceneBuilderName = "Speaker + Slides";

    [ObservableProperty]
    private string? _selectedParticipantId;

    [ObservableProperty]
    private bool _recording;

    [ObservableProperty]
    private bool _streaming;

    [ObservableProperty]
    private bool _masterLimiterEnabled = true;

    // The right "Video in room" panel is redundant with the Sources tab
    // (owner 2026-07-07: "probably not needed"); HIDDEN by default, revealed
    // via the "Room" toggle in the multiviewer header. Frees the width for
    // the multiviewer.
    [ObservableProperty]
    private bool _showRoomPanel;

    public string RoomPanelToggleLabel => ShowRoomPanel ? "Hide room" : "Show room";

    // Virtual camera (virtual-camera-spec V4): program as a system webcam.
    [ObservableProperty]
    private bool _virtualCameraEnabled;

    // V5: mirror-me self-view (flip the published frame horizontally).
    [ObservableProperty]
    private bool _virtualCameraMirror;

    // V5: operator-set camera display name (blank keeps the default).
    [ObservableProperty]
    private string _virtualCameraDeviceName = string.Empty;

    partial void OnVirtualCameraEnabledChanged(bool value)
    {
        OnPropertyChanged(nameof(VirtualCameraStatusLabel));
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnVirtualCameraMirrorChanged(bool value) => _ = TrySyncMediaCoreAsync();

    partial void OnVirtualCameraDeviceNameChanged(string value) => _ = TrySyncMediaCoreAsync();

    public string VirtualCameraStatusLabel
    {
        get
        {
            // Reflect operator intent first: if the operator enabled it, the pill
            // must never read "off" just because the snapshot hasn't reported
            // "live" yet (it reads "on"/"starting" until the core confirms).
            if (!VirtualCameraEnabled)
            {
                return "Virtual camera off";
            }
            var vc = _bridge.LastSnapshot?.VirtualCamera;
            return vc?.Status switch
            {
                "live" => $"Virtual camera live ({vc.Resolution.Width}x{vc.Resolution.Height})",
                "failed" => string.IsNullOrEmpty(vc.Warning) ? "Virtual camera failed" : vc.Warning,
                _ => "Virtual camera on"
            };
        }
    }

    /// <summary>Raw virtual-camera status from the core snapshot (off/starting/live/failed),
    /// for the control-API read model and diagnostics - distinct from the intent-aware label.</summary>
    public string VirtualCameraRawStatus => _bridge.LastSnapshot?.VirtualCamera?.Status ?? "(none)";

    partial void OnShowRoomPanelChanged(bool value) => OnPropertyChanged(nameof(RoomPanelToggleLabel));

    [ObservableProperty]
    private bool _masteringEnabled;

    [ObservableProperty]
    private int _masteringTargetIndex;  // 0=-14 streaming, 1=-16 podcast, 2=-23 broadcast

    [ObservableProperty]
    private double _masteringGlueAmount = 0.5;

    [ObservableProperty]
    private double _masteringCeilingDbfs = -1.3;

    [ObservableProperty]
    private double _masteringMaxRideDb = 12.0;

    [ObservableProperty]
    private double _masteringInputGainDb;

    [ObservableProperty]
    private double _masteringHighPassHz;

    [ObservableProperty]
    private double _masteringLowPassHz;

    [ObservableProperty]
    private double _masteringLowShelfDb;

    [ObservableProperty]
    private double _masteringPresenceDb;

    [ObservableProperty]
    private double _masteringHighShelfDb;

    [ObservableProperty]
    private double _masteringStereoWidth = 1.0;

    [ObservableProperty]
    private bool _audioMonitoringEnabled;

    [ObservableProperty]
    private string _selectedAudioMonitorDeviceId = string.Empty;

    [ObservableProperty]
    private double _audioMonitorVolume = 0.75;

    [ObservableProperty]
    private bool _localAudioSourceEnabled = true;

    [ObservableProperty]
    private string _selectedLocalAudioCaptureDeviceId = string.Empty;

    [ObservableProperty]
    private string _selectedAudioProcessingTargetId = "bus:master";

    [ObservableProperty]
    private string _ffmpegBinDirectory = Environment.GetEnvironmentVariable("COREVIDEO_FFMPEG_BIN_DIR") ??
        Environment.GetEnvironmentVariable("FFMPEG_BIN_DIR") ??
        string.Empty;

    [ObservableProperty]
    private bool _streamRtmpEnabled = true;

    [ObservableProperty]
    private bool _streamNdiEnabled;

    [ObservableProperty]
    private bool _streamSrtEnabled;

    [ObservableProperty]
    private string _streamRtmpProtocol = "rtmps";

    [ObservableProperty]
    private string _streamRtmpServerUrl = string.Empty;

    [ObservableProperty]
    private string _streamRtmpStreamKey = string.Empty;

    [ObservableProperty]
    private string _streamNdiProgramName = "CoreVideo Pro Program";

    [ObservableProperty]
    private string _streamNdiGroupName = "public";

    [ObservableProperty]
    private string _streamSrtMode = "caller";

    [ObservableProperty]
    private string _streamSrtHost = string.Empty;

    [ObservableProperty]
    private string _streamSrtPort = "9000";

    [ObservableProperty]
    private string _streamSrtLatencyMs = "120";

    [ObservableProperty]
    private string _streamSrtStreamId = string.Empty;

    [ObservableProperty]
    private string _streamSrtKeyLength = "0";

    [ObservableProperty]
    private string _streamSrtPassphrase = string.Empty;

    [ObservableProperty]
    private string _canvasResolution = MediaCoreProductionSyncContext.DefaultCanvasOutputProfile.Resolution;

    [ObservableProperty]
    private string _canvasFps = MediaCoreProductionSyncContext.DefaultCanvasOutputProfile.Fps.ToString();

    [ObservableProperty]
    private string _streamRenderResolution = MediaCoreProductionSyncContext.DefaultStreamOutputProfile.Resolution;

    [ObservableProperty]
    private string _streamRenderFps = MediaCoreProductionSyncContext.DefaultStreamOutputProfile.Fps.ToString();

    [ObservableProperty]
    private string _streamVideoCodec = MediaCoreProductionSyncContext.DefaultStreamOutputProfile.Codec;

    [ObservableProperty]
    private double _streamTargetBitrateMbps = MediaCoreProductionSyncContext.DefaultStreamOutputProfile.TargetBitrateMbps;

    [ObservableProperty]
    private double _streamAudioBitrateKbps = MediaCoreProductionSyncContext.DefaultStreamOutputProfile.AudioBitrateKbps;

    [ObservableProperty]
    private string _streamEncoderMode = "auto";

    [ObservableProperty]
    private string _streamPlatformProfileId = StreamingProfileCatalog.CustomProfileId;

    [ObservableProperty]
    private double _streamKeyframeIntervalSeconds = 2;

    [ObservableProperty]
    private string _streamRateControl = "cbr";

    [ObservableProperty]
    private string _streamH264Profile = "high";

    [ObservableProperty]
    private double _streamBFrames = 2;

    [ObservableProperty]
    private bool _streamAllowEnhancedRtmp;

    [ObservableProperty]
    private string _recordingRenderResolution = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.Resolution;

    [ObservableProperty]
    private string _recordingRenderFps = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.Fps.ToString();

    [ObservableProperty]
    private string _recordingVideoCodec = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.Codec;

    [ObservableProperty]
    private double _recordingTargetBitrateMbps = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.TargetBitrateMbps;

    [ObservableProperty]
    private double _recordingAudioBitrateKbps = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.AudioBitrateKbps;

    [ObservableProperty]
    private string _recordingTargetFolder = ResolveDefaultRecordingFolder();

    // Absolute default so recordings land somewhere the operator can find
    // (Videos\CoreVideo Pro) rather than the relative "Recordings/CoreVideo Pro" that
    // resolved next to the core exe. Overridable via the settings Browse picker.
    internal static string ResolveDefaultRecordingFolder()
    {
        try
        {
            var videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
            if (!string.IsNullOrWhiteSpace(videos))
            {
                return System.IO.Path.Combine(videos, "CoreVideo Pro");
            }
        }
        catch
        {
            // Fall back to the wire default below.
        }

        return MediaCoreProductionSyncContext.DefaultRecordingTargets.TargetFolder;
    }

    [ObservableProperty]
    private string _recordingFilenamePrefix = MediaCoreProductionSyncContext.DefaultRecordingTargets.FilenamePrefix;

    [ObservableProperty]
    private string _recordingFormat = MediaCoreProductionSyncContext.DefaultRecordingTargets.Format;

    [ObservableProperty]
    private string _recordingQuality = MediaCoreProductionSyncContext.DefaultRecordingTargets.Quality;

    // Multiviewer operator preferences. These drive the configure-multiviewer command; the overlay
    // and core agents bind/consume these exact property names.
    [ObservableProperty]
    private string _multiviewLayoutMode = DefaultMultiviewLayoutMode;

    [ObservableProperty]
    private int _multiviewTileCount = DefaultMultiviewTileCount;

    [ObservableProperty]
    private bool _multiviewShowLabels = true;

    [ObservableProperty]
    private bool _multiviewShowTally = true;

    [ObservableProperty]
    private bool _multiviewShowMeters = true;

    [ObservableProperty]
    private bool _multiviewShowClock;

    [ObservableProperty]
    private string _outputStatus = "Outputs idle";

    [ObservableProperty]
    private string _outputSessionStatus = "Outputs idle";

    [ObservableProperty]
    private string _commandStatus = "Program ready";

    [ObservableProperty]
    private string _captionSpeaker = string.Empty;

    [ObservableProperty]
    private string _captionText = string.Empty;

    [ObservableProperty]
    private string _lowerThirdName = string.Empty;

    [ObservableProperty]
    private string _lowerThirdTitle = string.Empty;

    [ObservableProperty]
    private string _lowerThirdOrg = string.Empty;

    [ObservableProperty]
    private LowerThirdKeyState _programLowerThirdKey = LowerThirdKeyState.Hidden();

    [ObservableProperty]
    private bool _programLowerThirdEnabled;

    [ObservableProperty]
    private double _lowerThirdBuildInMs = 220;

    [ObservableProperty]
    private double _lowerThirdBuildOutMs = 160;

    [ObservableProperty]
    private string _selectedLowerThirdTimingPreset = "Quick";

    public IReadOnlyList<string> LowerThirdTimingPresetOptions { get; } =
        ["Quick", "Smooth", "Gentle", "Custom"];

    [ObservableProperty]
    private VideoSurfaceState _programSurface = VideoSurfaceState.Slate(VideoSurfaceKind.Program, "program", "Program");

    [ObservableProperty]
    private VideoSurfaceState _previewSurface = VideoSurfaceState.Slate(VideoSurfaceKind.Preview, "preview", "Preview");

    [ObservableProperty]
    private IReadOnlyList<ParticipantSurfaceTile> _multiviewTiles = [];

    [ObservableProperty]
    private IReadOnlyList<ParticipantSurfaceTile> _multiviewGridTiles = [];

    // The ONE core-composited GPU multiview shared-texture surface + its normalized tile rects
    // (for the transparent click overlay). Replaces the per-tile CPU multiview path.
    [ObservableProperty]
    private VideoSurfaceState _multiviewSurface =
        VideoSurfaceState.Slate(VideoSurfaceKind.Multiview, "multiview", "Multiview");

    [ObservableProperty]
    private IReadOnlyList<CoreVideoPro.MediaCore.Models.MultiviewTile> _multiviewTileRects = [];

    // Pop-out: the multiviewer is a SINGLE-consumer keyed-mutex GPU texture, so when it is
    // popped out to its own window the main view must UNBIND it (present null) — only the pop-out
    // window may present it, or the texture wedges (the old preview-freeze/CoreMessagingXP class).
    [ObservableProperty]
    private bool _multiviewPoppedOut;

    // The surface the MAIN Studio multiviewer presents: null while popped out.
    public VideoSurfaceState? StudioMultiviewSurface => MultiviewPoppedOut ? null : MultiviewSurface;

    public Microsoft.UI.Xaml.Visibility MultiviewPoppedOutVisibility =>
        MultiviewPoppedOut ? Microsoft.UI.Xaml.Visibility.Visible : Microsoft.UI.Xaml.Visibility.Collapsed;

    partial void OnMultiviewSurfaceChanged(VideoSurfaceState value) =>
        OnPropertyChanged(nameof(StudioMultiviewSurface));

    partial void OnMultiviewPoppedOutChanged(bool value)
    {
        OnPropertyChanged(nameof(StudioMultiviewSurface));
        OnPropertyChanged(nameof(MultiviewPoppedOutVisibility));
    }

    public ObservableCollection<ShowInputSlot> ShowInputs { get; } =
        new(ShowInputRosterService.CreateDefaultSlots());

    public ObservableCollection<ShowInputSlotViewModel> ShowInputEditors { get; } = [];

    public ObservableCollection<SrtIngestSource> SrtIngestSources { get; } =
    [
        CreateSrtIngestSource(1)
    ];

    [ObservableProperty]
    private ProductionMode _productionMode = ProductionMode.Manual;

    [ObservableProperty]
    private string _magicSceneStatus = "Join a meeting to enable Magic Scene";

    [ObservableProperty]
    private string _autoProductionReadout = "Join a Zoom meeting to enable scene recommendations.";

    [ObservableProperty]
    private string _automationButtonLabel = "Automation disabled";

    [ObservableProperty]
    private bool _automationAutoTakeEnabled = true;

    [ObservableProperty]
    private bool _automationPreferScreenShare = true;

    [ObservableProperty]
    private bool _automationLowerThirdsEnabled = true;

    // When on, newly-joined Zoom participants auto-fill FREE Show Input slots (never
    // disturbing operator- or capture-assigned slots). See SyncShowInputsFromMeeting.
    [ObservableProperty]
    private bool _automationAutoAssignInputsEnabled = true;

    [ObservableProperty]
    private bool _automationCaptionsEnabled = true;

    [ObservableProperty]
    private string _takeTransitionMode = "fade";

    [ObservableProperty]
    private double _automationConfidenceThreshold = 70;

    [ObservableProperty]
    private double _automationSwitchDelaySeconds = 4;

    [ObservableProperty]
    private double _automationPanelParticipantThreshold = 4;

    [ObservableProperty]
    private string _automationLastAction = "Automation is idle";

    [ObservableProperty]
    private ColorGrade _colorGrade = ProductionCatalog.ColorGrade;

    [ObservableProperty]
    private string _mediaBinSummary = "Media bin is empty";

    [ObservableProperty]
    private string _mediaBinGuidance = string.Empty;

    [ObservableProperty]
    private string? _selectedMediaAssetId;

    [ObservableProperty]
    private string? _selectedMediaAssetName;

    [ObservableProperty]
    private string? _selectedMediaAssetPath;

    [ObservableProperty]
    private string? _selectedMediaAssetKind;

    [ObservableProperty]
    private bool _selectedMediaAssetPlaying;

    [ObservableProperty]
    private string _mediaPlaybackStatus = "No media asset playing";

    [ObservableProperty]
    private string _captureFleetSummary = "No video capture devices detected";

    [ObservableProperty]
    private string _captureDevicesEmptyGuidance = ProductionStateHelper.CaptureDevicesEmptyGuidance();

    [ObservableProperty]
    private bool _dualCaptureLive;

    [ObservableProperty]
    private IReadOnlyList<ShowInputSourceOption> _dualCaptureSourceOptions = [];

    [ObservableProperty]
    private string? _primaryCaptureDeviceId;

    [ObservableProperty]
    private string? _secondaryCaptureDeviceId;

    [ObservableProperty]
    private string _dualCaptureSummary = "Choose capture sources to enable dual capture.";

    [ObservableProperty]
    private string _feedHealthSummary = "No Zoom feeds — join a meeting";

    [ObservableProperty]
    private string _previewSceneParticipants = "No sources assigned";

    private readonly Dictionary<string, List<SourceRoute>> _sceneRoutes = new(StringComparer.Ordinal);
    private readonly Dictionary<string, string> _sceneBackgroundAssetIds = new(StringComparer.Ordinal);
    // Operator-overridden source display names keyed by canonical source id
    // (zoom:<pid> / capture:<id> / media:<id>). Feeds the auto lower-thirds and
    // multiview labels; absent keys fall back to the derived Zoom/UVC/asset name.
    private readonly Dictionary<string, string> _sourceDisplayNames = new(StringComparer.Ordinal);
    // Per-source color grades keyed by participant id or capture:<deviceId>.
    private readonly Dictionary<string, ColorGrade> _sourceColorGrades = new(StringComparer.Ordinal);
    private string? _automationPendingSceneId;
    private DateTimeOffset? _automationPendingSince;
    private bool _automationTakeInFlight;
    private bool _previewRoutingRefreshScheduled;
    private bool _showInputRefreshScheduled;
    private int _programMediaPlaybackTakeVersion;
    private readonly IShowInputRosterStore _showInputRosterStore = CreateShowInputRosterStore();
    private bool _showInputRosterLoaded;
    private bool _multiviewGridRefreshScheduled;
    private bool _canvasInteractionActive;
    private bool _refreshingSceneBackgroundSelection;
    private bool _applyingDualCaptureSelection;
    private bool _recordingToggleInFlight;
    private bool _streamToggleInFlight;
    private readonly HashSet<string> _captureAutoConnectInFlight = new(StringComparer.Ordinal);
    private readonly SemaphoreSlim _captureConnectGate = new(1, 1);

    public SettingsViewModel Settings { get; }

    public TransportViewModel Transport { get; }

    public OverlaysViewModel Overlays { get; }

    public ObservableCollection<GraphicOverlay> Graphics { get; } = [];

    public ObservableCollection<CaptureDevice> CaptureDevices { get; } = [];

    public IReadOnlyList<CaptureDevice> BrowserCaptureDevices => CaptureDevices
        .Where(device => device.Id.StartsWith("browser:", StringComparison.Ordinal))
        .ToList();

    public bool HasBrowserCaptureDevices => BrowserCaptureDevices.Count > 0;

    private readonly HashSet<string> _onAirBrowserOverlayIds = new(StringComparer.Ordinal);
    private readonly HashSet<string> _previewBrowserOverlayIds = new(StringComparer.Ordinal);
    private string? _primaryBrowserOverlayId;

    private CaptureDevice? PrimaryBrowserOverlay =>
        BrowserCaptureDevices.FirstOrDefault(device =>
            string.Equals(device.Id, _primaryBrowserOverlayId, StringComparison.Ordinal)) ??
        BrowserCaptureDevices.FirstOrDefault();

    public string StudioBrowserOverlayButtonLabel =>
        PrimaryBrowserOverlay?.IsBrowserOverlayOnAir == true ? "DSK out" : "DSK in";

    public string StudioBrowserOverlayPreviewButtonLabel =>
        PrimaryBrowserOverlay?.IsBrowserOverlayInPreview == true ? "Clear PVW" : "Preview";

    public string StudioBrowserOverlayPreviewStatus =>
        PrimaryBrowserOverlay?.IsBrowserOverlayInPreview == true ? "Staged" : "Off";

    public string StudioBrowserOverlayStatus =>
        PrimaryBrowserOverlay?.IsBrowserOverlayOnAir == true ? "On air" : "Ready";

    public string StudioBrowserOverlayToolTip => PrimaryBrowserOverlay is { } overlay
        ? $"DSK browser overlay: {overlay.Name}"
        : "Add a browser overlay source first";

    public ObservableCollection<AudioCaptureDevice> AudioCaptureDevices { get; } = [];

    public ObservableCollection<AudioRenderDevice> AudioRenderDevices { get; } = [];

    public IReadOnlyList<ShowInputSourceOption> AudioCaptureSourceOptions =>
        ShowInputRosterService.BuildAudioSourceOptions(AudioCaptureDevices);

    public IReadOnlyList<FeedHealthRow> FeedHealthRows { get; private set; } = [];

    [ObservableProperty]
    private BrandKit _brandKit = ProductionCatalog.BrandKit;

    public IReadOnlyList<CaptionTranscriptEntry> CaptionTranscript { get; private set; } = [];

    private readonly List<GraphicsOverlaySync.CaptionTranscriptEntryPatch> _captionTranscriptPatches = [];

    public IReadOnlyList<MediaBinGroup> MediaBinGroups { get; private set; } = [];

    public void SetBrandKit(BrandKit brandKit)
    {
        BrandKit = brandKit;
        ApplyBrandOverlayDefaults(brandKit.DefaultOverlayBehavior);
        Overlays?.NotifyBrandKitChanged();
        SaveProductionOutputPreferences();
        _ = TrySyncMediaCoreAsync();
    }

    public void NotifyLowerThirdPresentationChanged()
    {
        SaveProductionOutputPreferences();
        RefreshProgramLowerThirdKeyPosition();
    }

    public bool ApplySelectedMediaAssetAsBrandLogo()
    {
        if (SelectedMediaAssetId is null || FindMediaAsset(SelectedMediaAssetId) is not { } asset)
        {
            return false;
        }

        SetBrandKit(new BrandKit
        {
            Name = BrandKit.Name,
            LogoText = BrandKit.LogoText,
            LogoAssetId = asset.Id,
            LogoAssetName = asset.Name,
            LogoAssetPath = asset.FilePath,
            BrandColor = BrandKit.BrandColor,
            AccentColor = BrandKit.AccentColor,
            BackgroundColor = BrandKit.BackgroundColor,
            FontFamily = BrandKit.FontFamily,
            LowerThirdStyle = BrandKit.LowerThirdStyle,
            CaptionStyle = BrandKit.CaptionStyle,
            DefaultOverlayBehavior = BrandKit.DefaultOverlayBehavior
        });
        CommandStatus = $"{asset.Name} assigned as brand logo";
        return true;
    }

    private void ApplyBrandOverlayDefaults(string behavior)
    {
        if (string.Equals(behavior, "manual", StringComparison.Ordinal))
        {
            return;
        }

        foreach (var graphic in Graphics)
        {
            graphic.Enabled = behavior switch
            {
                "all-off" => false,
                _ => graphic.Enabled
            };
        }

        OnPropertyChanged(nameof(EnabledGraphics));
    }

    private readonly List<ParticipantAudioMix> _audioMixChannels = [];
    private AutoProductionState _automationRecommendation = ProductionStateHelper.BuildAutomationRecommendation([], ProductionCatalog.Scenes);

    public ObservableCollection<AudioParticipantRow> AudioParticipantRows { get; } = [];

    public IReadOnlyList<AudioProcessingTargetOption> AudioProcessingTargetOptions { get; private set; } = [];
    private string _audioProcessingTargetsSignature = string.Empty;

    public string CaptionQualitySummary =>
        ProductionStateHelper.CaptionQualitySummary(
            !string.IsNullOrWhiteSpace(CaptionText) || CaptionTranscript.Count > 0,
            CaptionTranscript.Count);

    public ObservableCollection<SceneCanvasLayerViewModel> PreviewCanvasLayers { get; } = [];

    /// <summary>
    /// "Add source" picker for the scene canvas (§4 IA): Inputs 1-10 plus the auto roles
    /// (Active Speaker / Screen Share / Media). Sourced from <see cref="SceneRoutingService"/>
    /// so the layer references an Input directly.
    /// </summary>
    public IReadOnlyList<RouteSelectOption> AddSourceOptions { get; } = SceneRoutingService.AddSourceOptions;

    public IReadOnlyList<RouteSelectOption> SuperSourceBackgroundOptions =>
        new[]
            {
                new RouteSelectOption { Value = SceneBackgroundSelectionService.NoBackgroundValue, Label = "No background" }
            }
            .Concat(MediaBinGroups
                .SelectMany(group => group.Assets)
                .Where(IsVisualMediaAsset)
                .OrderBy(asset => asset.Name, StringComparer.OrdinalIgnoreCase)
                .Select(asset => new RouteSelectOption
                {
                    Value = asset.Id,
                    Label = $"{asset.Name} ({asset.Kind})"
                }))
            .ToList();

    [ObservableProperty]
    private string _previewSceneBackgroundAssetId = SceneBackgroundSelectionService.NoBackgroundValue;

    public MediaAsset? PreviewSceneBackgroundAsset => ResolveSceneBackgroundAsset(PreviewSceneId);

    public MediaAsset? ProgramSceneBackgroundAsset => ResolveSceneBackgroundAsset(ActiveSceneId);

    public string SuperSourceBackgroundSummary =>
        PreviewSceneBackgroundAsset is { } asset
            ? $"{asset.Name} is behind {PreviewScene.Name}"
            : $"No SuperSource background for {PreviewScene.Name}";

    private IReadOnlyList<MediaAsset> VisualMediaAssets =>
        MediaBinGroups
            .SelectMany(group => group.Assets)
            .Where(IsVisualMediaAsset)
            .ToList();

    // POS-2: the Add-overlay flyouts (Studio preview header + Scenes tab) list the
    // visual media assets directly — logos/bugs are the primary overlay case.
    public IReadOnlyList<MediaAsset> OverlayMediaAssets => VisualMediaAssets;

    public AudioRoutingMatrixViewModel AudioRoutingMatrix { get; } = new();

    public VideoRoutingMatrixViewModel VideoRoutingMatrix { get; } = new();

    // U2 consolidation: the Routing tab is video-only; audio routing lives
    // solely on the Audio tab's SETUP surface. The old "audio"/"video"
    // RoutingMatrixMode plumbing is gone with the dual-hosting.

    public IReadOnlyList<SourceRoute> PreviewSceneRoutes { get; private set; } = [];

    public IReadOnlyList<SourceRoute> ProgramSceneRoutes { get; private set; } = [];

    public IReadOnlyList<string> PreviewRouteWarnings { get; private set; } = [];

    public bool HasPreviewRouteWarnings => PreviewRouteWarnings.Count > 0;

    public IReadOnlyList<ParticipantSurfaceTile> PreviewSceneTiles { get; private set; } = [];

    public IReadOnlyList<ParticipantSurfaceTile> ProgramSceneTiles { get; private set; } = [];

    public string ProgramSceneSummary => ResolveOnAirSceneLabel(ActiveSceneId);

    public string PreviewSceneSummary => ResolveOnAirSceneLabel(PreviewSceneId);

    public string LowerThirdKeyStatus =>
        $"{ProgramLowerThirdKey.PhaseLabel} - {ProgramLowerThirdKey.SourceLabel}";

    public string LowerThirdKeySummary =>
        ProgramLowerThirdKey.IsVisible
            ? $"{ProgramLowerThirdKey.SourceName} keyed from program source"
            : "Lower-third key follows the active program source.";

    public string StudioLowerThirdButtonLabel =>
        FormatStudioLowerThirdActionLabel(ProgramLowerThirdKey.IsVisible);

    public string StudioLowerThirdCompactLabel =>
        FormatStudioLowerThirdActionLabel(ProgramLowerThirdKey.IsVisible);

    public string StudioLowerThirdCompactStatus =>
        ProgramLowerThirdKey.IsVisible
            ? FormatLowerThirdPhaseLabel(ProgramLowerThirdKey.Phase)
            : ResolveProgramLowerThirdSource(ProgramSceneRoutes) is not null
                ? "Ready"
                : "No source";

    public string StudioLowerThirdSourceLabel =>
        ProgramLowerThirdKey.IsVisible
            ? $"{ProgramLowerThirdKey.SourceName} - {ProgramLowerThirdKey.PhaseLabel}"
            : ResolveProgramLowerThirdSource(ProgramSceneRoutes) is { } source
                ? $"Ready: {source.SourceName}"
                : "Take a scene with a source first";

    public string StudioLowerThirdToolTip =>
        $"{StudioLowerThirdSourceLabel} - {LowerThirdTimingSummary}";

    public string NativeLowerThirdStatus =>
        _bridge.LastSnapshot?.OverlayState is { } overlay
            ? FormatNativeLowerThirdStatus(overlay)
            : "Native lower-third state waiting for media core.";

    public string LowerThirdTimingSummary =>
        $"{SelectedLowerThirdTimingPreset} · in {LowerThirdBuildInMs:0} ms / out {LowerThirdBuildOutMs:0} ms";

    public string LoudnessTargetLabel => "target -16 LUFS";

    public string LoudnessLevelLabel =>
        _bridge.LastSnapshot?.AudioMixSession is { } audio
            ? audio.LimiterActive
                ? "limiting"
                : audio.LoudnessLufs <= -59
                    ? "awaiting audio"
                    : audio.LoudnessLufs < -18
                        ? "below target"
                        : audio.LoudnessLufs > -14
                            ? "above target"
                            : "on target"
            : "awaiting audio";

    public string TruePeakLabel => "—";

    public string GainAdjustLabel => SelectedGainLabel;

    public string ClipTrimSummary => "No clip selected";

    public string ChapterSummary => "No chapters";

    public string SceneIntelligenceSummary =>
        ProductionStateHelper.BuildSceneIntelligenceSummary(RoomVideoParticipants, ProductionMode);

    public string RecommendedSceneName =>
        ProductionStateHelper.RecommendedSceneName(Scenes, _automationRecommendation.RecommendedSceneId);

    public string RecommendedLayout =>
        ProductionStateHelper.RecommendedLayout(Scenes, _automationRecommendation.RecommendedSceneId);

    public string RecommendedConfidence =>
        _automationRecommendation.Confidence > 0 ? $"{_automationRecommendation.Confidence}%" : "—";

    public string AutoSwitchLabel => ProductionMode == ProductionMode.SetAndForget ? "Auto" : "Manual";

    public string AutomationPolicySummary =>
        $"{(AutomationAutoTakeEnabled ? "Auto-take" : "Queue preview")} - " +
        $"{AutomationConfidenceThreshold:0}% confidence - {AutomationSwitchDelaySeconds:0}s hold";

    public string AutomationScenePolicySummary =>
        $"{(AutomationPreferScreenShare ? "Prefer screen share" : "Ignore screen share")} - " +
        $"panel at {AutomationPanelParticipantThreshold:0}+ sources";

    public string AutomationOverlayPolicySummary =>
        $"{(AutomationLowerThirdsEnabled ? "Lower thirds on" : "Lower thirds off")} - " +
        $"{(AutomationCaptionsEnabled ? "Captions on" : "Captions off")}";

    public string AutomationTakeModeLabel => AutomationAutoTakeEnabled ? "Take to program" : "Queue on preview";

    public int CamerasOnCount => RoomVideoParticipants.Count;

    public string ScreenShareLabel => RoomVideoParticipants.Any(p => p.IsScreenSharing) ? "Active" : "Off";

    public string AutoProductionReason => _automationRecommendation.Reason;

    public AudioMixState AudioMix => new()
    {
        Participants = _audioMixChannels,
        LoudnessLufs = _bridge.LastSnapshot?.AudioMixSession.LoudnessLufs ?? -16,
        LimiterEnabled = MasterLimiterEnabled,
        LimiterActive = _bridge.LastSnapshot?.AudioMixSession.LimiterActive ?? false,
        Summary = ProductionStateHelper.BuildAudioMixSummary(RoomVideoParticipants)
    };

    public string MasterLimiterModeLabel => MasterLimiterEnabled ? "Limiter enabled" : "Limiter bypassed";

    public string MasterLimiterActivityLabel =>
        !MasterLimiterEnabled ? "Bypassed" : AudioMix.LimiterActive ? "Reducing peaks" : "Standing by";

    public double MasteringTargetLufs => MasteringTargetIndex switch { 1 => -16.0, 2 => -23.0, _ => -14.0 };

    public string MasteringActivityLabel =>
        !MasteringEnabled
            ? "Bypassed"
            : $"Riding {_bridge.LastSnapshot?.AudioMixSession.MasteringRideDb ?? 0:+0.0;-0.0} dB toward {MasteringTargetLufs:0} LUFS";

    public string MasteringCompareSlot => _masteringCompareSlot;

    public bool IsMasteringCompareA => string.Equals(_masteringCompareSlot, "A", StringComparison.Ordinal);

    public bool IsMasteringCompareB => string.Equals(_masteringCompareSlot, "B", StringComparison.Ordinal);

    public string MasterLimiterSummary =>
        MasterLimiterEnabled
            ? "Master protection is armed; activity only lights when peaks reach the ceiling."
            : "Master protection is bypassed; no limiting is applied.";

    public string SelectedAudioMonitorDeviceName =>
        AudioRenderDevices.FirstOrDefault(device =>
            string.Equals(device.Id, SelectedAudioMonitorDeviceId, StringComparison.Ordinal))?.Name ??
        "No monitor output selected";

    private string SelectedAudioMonitorNativeDeviceId =>
        AudioRenderDevices.FirstOrDefault(device =>
            string.Equals(device.Id, SelectedAudioMonitorDeviceId, StringComparison.Ordinal))?.NativeDeviceId ??
        string.Empty;

    public string AudioMonitorVolumeLabel => $"{AudioMonitorVolume * 100:0}%";

    public string AudioMonitorStatus =>
        AudioMonitoringEnabled
            ? $"Monitor target - {SelectedAudioMonitorDeviceName}"
            : "Monitor muted";

    public string AudioMonitorEngineStatus =>
        _bridge.LastSnapshot is { } snapshot
            ? FormatAudioMonitorEngineStatus(snapshot.AudioMixSession, snapshot.CaptureAudioSources)
            : "Waiting for media engine audio telemetry.";

    public string AudioProofSummary =>
        _bridge.LastSnapshot is { } snapshot
            ? FormatAudioProofSummary(
                snapshot.AudioMixSession,
                snapshot.CaptureAudioSources,
                snapshot.OutputSenderSession,
                snapshot.Recording)
            : "Audio proof waiting for media core.";

    public string AudioValidationSummary =>
        _bridge.LastSnapshot is { } snapshot
            ? AudioValidationChecklistBuilder.SummarizeAcceptance(
                snapshot.AudioMixSession,
                snapshot.CaptureAudioSources,
                snapshot.OutputSenderSession,
                snapshot.Recording)
            : "Audio validation waiting for media core.";

    public string AudioValidationChecklist =>
        _bridge.LastSnapshot is { } snapshot
            ? AudioValidationChecklistBuilder.Format(
                snapshot.AudioMixSession,
                snapshot.CaptureAudioSources,
                snapshot.OutputSenderSession,
                snapshot.Recording)
            : "CAPTURE idle | PGM wait PCM | MON off | STREAM idle | RECORD idle";

    public string AudioValidationFirstIssue =>
        _bridge.LastSnapshot is { } snapshot
            ? AudioValidationChecklistBuilder.FindFirstBlockingStage(
                snapshot.AudioMixSession,
                snapshot.CaptureAudioSources,
                snapshot.OutputSenderSession,
                snapshot.Recording)
            : "First audio block: waiting for media core.";

    public string AudioFullChainValidationSummary =>
        _bridge.LastSnapshot is { } snapshot
            ? AudioValidationChecklistBuilder.SummarizeFullChain(
                snapshot.AudioMixSession,
                snapshot.CaptureAudioSources,
                snapshot.OutputSenderSession,
                snapshot.Recording)
            : "Full audio chain incomplete: media core not yet proven.";

    public string AudioMeterSourceSummary =>
        _bridge.LastSnapshot is { } snapshot
            ? BuildAudioMeterSourceSummary(snapshot.AudioMixSession, snapshot.CaptureAudioSources)
            : "Meters show media-core program/master bus telemetry when the engine is running.";

    public string CaptureAudioSignalSummary =>
        _bridge.LastSnapshot?.CaptureAudioSources is { SourceCount: > 0 } capture
            ? BuildCaptureAudioSignalSummary(capture)
            : _bridge.LastSnapshot?.CaptureAudioSources is { } idle
                ? idle.Summary
                : "Capture audio telemetry waiting for media core.";

    public string AudioBusTapSummary =>
        _bridge.LastSnapshot?.AudioRoutingMatrix is { } matrix
            ? BuildAudioBusTapSummary(matrix)
            : "Audio bus tap telemetry waiting for media core.";

    public string CaptureAudioSourceSummary =>
        _bridge.LastSnapshot?.CaptureAudioSources is { Sources.Count: > 0 } capture
            ? string.Join(" | ", capture.Sources.Take(3).Select(FormatCaptureAudioSourceStatus))
            : "No paired local/capture audio sources reported by native core.";

    public string NativeCoreRuntimeStatus =>
        FormatNativeCoreRuntimeStatus(MediaCorePaths.ResolveNativeCoreExecutable(), _bridge.Profile);

    public string NativeAudioRuntimeStatus =>
        FormatNativeAudioRuntimeStatus(_bridge.Profile);

    public string StudioMonitorSummary =>
        _bridge.LastSnapshot is { } snapshot
            ? FormatStudioMonitorSummary(
                snapshot.AudioMixSession,
                snapshot.CaptureAudioSources,
                AudioMonitoringEnabled,
                SelectedAudioMonitorDeviceName)
            : "Monitor waiting for media engine";

    public string SelectedLocalAudioCaptureDeviceName =>
        AudioCaptureDevices.FirstOrDefault(device =>
            string.Equals(device.Id, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal))?.DisplayLabel ??
        "No local audio input selected";

    public string LocalAudioSourceStatus =>
        LocalAudioSourceEnabled
            ? ResolveLocalAudioSourceStatus()
            : "Local machine audio source disabled";

    public string LocalAudioOperatorStatus =>
        LocalAudioSourceEnabled
            ? ResolveLocalAudioOperatorStatus()
            : "Local audio source off";

    public string LocalAudioSourceRecommendation =>
        LocalAudioSourceEnabled
            ? FormatLocalAudioSourceRecommendation(
                SelectedLocalAudioCaptureDeviceId,
                SelectedLocalAudioCaptureDeviceName,
                AudioCaptureDevices,
                _bridge.LastSnapshot?.CaptureAudioSources)
            : "Enable Source to capture local machine audio.";

    public string FfmpegRuntimeStatus
    {
        get
        {
            var executable = StudioStreamOutputValidation.ResolveFfmpegExecutable(FfmpegBinDirectory);
            if (!string.IsNullOrWhiteSpace(executable))
            {
                return $"FFmpeg runtime found: {executable}";
            }

            if (!Directory.Exists(FfmpegBinDirectory))
            {
                return string.IsNullOrWhiteSpace(FfmpegBinDirectory)
                    ? "FFmpeg not configured. RTMP/RTMPS needs ffmpeg.exe in the app folder, PATH, or a selected bin folder."
                    : "FFmpeg folder not found. Choose the bin folder that contains ffmpeg.exe.";
            }

            return "Folder must contain ffmpeg.exe. Choose the FFmpeg bin folder, not the install root.";
        }
    }

    public string FfmpegOperatorStatus =>
        StudioStreamOutputValidation.ResolveFfmpegExecutable(FfmpegBinDirectory) is not null
            ? "Streaming encoder ready."
            : "Streaming encoder not found. Choose the folder containing ffmpeg.exe.";

    public StudioViewModel()
    {
        ExternalUriLauncher.BindDispatcher(Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
        AudioRoutingMatrix.RouteChanged += OnAudioRoutingMatrixChanged;
        AudioRoutingMatrix.BusOutputsChanged += () => _ = TrySyncMediaCoreAsync();
        VideoRoutingMatrix.RouteChanged += OnVideoRoutingMatrixChanged;
        SrtIngestSources.CollectionChanged += OnSrtIngestSourcesChanged;
        foreach (var source in SrtIngestSources)
        {
            source.PropertyChanged += OnSrtIngestSourcePropertyChanged;
        }

        _currentRoomId = "main";
        _currentRoomName = "Main room";

        _scenes = new ObservableCollection<Scene>(ProductionCatalog.Scenes);
        RoomVideoParticipants = [];
        CurrentRoomLabel = "No meeting";
        _multiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
        LoadShowInputRoster();
        InitializeShowInputEditors();
        RefreshMultiviewGridTiles();
        RefreshParticipantListItems();
        RefreshSceneItems();
        RefreshAudioParticipantRows();

        _zoomOAuth = new ZoomOAuthService(
            new FileZoomTokenStore(FileZoomTokenStore.DefaultTokenStorePath()),
            openUrl: ExternalUriLauncher.OpenAsync);
        _zoomOAuthCoordinator = new ZoomOAuthAppCoordinator(
            _zoomOAuth,
            Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
        _zoomOAuthCoordinator.Initialize();
        if (Environment.ProcessPath is { } exePath)
        {
            _zoomOAuthCoordinator.TryRegisterProtocolHandler(exePath, out _);
        }

        Settings = new SettingsViewModel(
            _bridge,
            () => ZoomCaptureSubscribed,
            _zoomOAuth,
            drainOAuthCallback: () => _zoomOAuthCoordinator.TryDrainPendingCallback(),
            // These callbacks update data-bound VM state and may be invoked by the
            // SettingsViewModel from a background thread; updating a bound property
            // off the UI thread crashes WinUI (RPC_E_WRONG_THREAD / 0xc000027b).
            onMeetingPresenceChanged: () => RunOnUiThread(() =>
            {
                OnPropertyChanged(nameof(CanToggleCapture));
                OnPropertyChanged(nameof(CanToggleRecording));
                OnPropertyChanged(nameof(CaptureEngineHint));
                NotifyShowReadinessChanged();
                ToggleEngineCommand.NotifyCanExecuteChanged();
                ToggleRecordingCommand.NotifyCanExecuteChanged();
            }),
            onBeforeLeaveMeeting: () =>
            {
                RunOnUiThread(() =>
                {
                    if (ZoomCaptureSubscribed)
                    {
                        UnsubscribeZoomCapture("Capture off — leaving meeting");
                    }
                });

                return Task.CompletedTask;
            },
            zoomStatusChanged: status => RunOnUiThread(() => ZoomStatus = status),
            onMeetingJoined: () => RunOnUiThread(() => ActiveTab = StudioTab.Studio));
        Settings.ConfigureOutputDestinations(BuildSupportBundleOutputDestinations);
        _zoomOAuthCoordinator.SetStatusChangedHandler(message =>
        {
            Settings.OauthStatusMessage = message;
            _ = Settings.RefreshOAuthStatusAsync();
        });
        _zoomOAuthCoordinator.TryDrainPendingCallback();
        Transport = new TransportViewModel();
        Overlays = new OverlaysViewModel(this);
        LoadProductionOutputPreferences();
        _automationTimer = _dispatcher.CreateTimer();
        _automationTimer.Interval = TimeSpan.FromMilliseconds(500);
        _automationTimer.Tick += (_, _) => EvaluateAutomationPolicy();
        InitializeGraphicsCatalog();
        InitializeSceneRoutes();
        RefreshPreviewRoutingState();
        RefreshMediaBin();
        RefreshProductionReadouts();
        RefreshTransportState();

        _bridge.HealthChanged += OnBridgeHealthChanged;
        _bridge.StatusChanged += OnBridgeStatusChanged;
        _bridge.ProfileChanged += OnBridgeProfileChanged;
        _bridge.SnapshotChanged += OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived += OnZoomVideoFrameReceived;
        _bridge.ProgramFramePreviewReceived += OnProgramFramePreviewReceived;
        _bridge.ProgramSharedTextureReceived += OnProgramSharedTextureReceived;
        _bridge.PreviewSharedTextureReceived += OnPreviewSharedTextureReceived;
        _bridge.ParticipantSharedTextureReceived += OnParticipantSharedTextureReceived;
        _bridge.MultiviewSharedTextureReceived += OnMultiviewSharedTextureReceived;
        CaptureDeviceFrameRouter.FrameReceived += OnCaptureDeviceFrameReceived;
        _surfaces.SurfacesChanged += OnSurfacesChanged;

        MediaBinGuidance = MediaBinClassifier.BuildEmptyGuidanceMessage();
        _captureDiscovery.StartWatching(() => _ = RefreshCaptureDevicesAsync());
        _audioCaptureDiscovery.StartWatching(() => _ = RefreshAudioCaptureDevicesAsync());
        _audioRenderDiscovery.StartWatching(() =>
        {
            _ = RefreshAudioRenderDevicesAsync();
            _ = RefreshAudioCaptureDevicesAsync();
        });
        _ = RefreshCaptureDevicesAsync();
        _ = RefreshAudioCaptureDevicesAsync();
        _ = RefreshAudioRenderDevicesAsync();
        _ = StartMediaCoreOnLaunchAsync();
    }

    private readonly ObservableCollection<Scene> _scenes;

    public IReadOnlyList<Scene> Scenes => _scenes;

    public IReadOnlyList<SceneDisplayItem> SceneItems { get; private set; } = [];

    public IReadOnlyList<Participant> RoomVideoParticipants { get; private set; }

    // All in-room participants (incl. video-off) for the Sources/Inputs picker.
    public IReadOnlyList<Participant> RoomParticipantsForInputs { get; private set; } = [];

    public IReadOnlyList<Participant> SceneParticipants => RoomVideoParticipants;

    public Scene ProgramScene => Scenes.First(s => s.Id == ActiveSceneId);

    public Scene PreviewScene => Scenes.First(s => s.Id == PreviewSceneId);

    [ObservableProperty]
    private string _currentRoomLabel;

    // Design language: label stays "Capture"; the pill's colour + dot show on/off.
    public string EngineRunningLabel => "Capture";

    public bool CanToggleCapture => Settings.IsInMeeting;

    // Recording rights can be requested in a breakout room without capture running,
    // so recording only requires being in a meeting (NOT an active capture subscription).
    public bool CanToggleRecording => Settings.IsInMeeting && !_recordingToggleInFlight;

    public string CaptureEngineHint => CanToggleCapture
        ? "Start or stop Zoom video capture for this meeting."
        : "Join a Zoom meeting before starting capture.";

    public string RecordingLabel => Recording ? "Recording" : "Record";

    public string StreamingLabel => Streaming ? "Streaming" : "Stream";

    public IReadOnlyList<string> StreamRtmpProtocolOptions { get; } = ["rtmps", "rtmp"];

    public IReadOnlyList<RouteSelectOption> StreamPlatformProfileOptions { get; } =
        StreamingProfileCatalog.Profiles
            .Select(profile => new RouteSelectOption { Value = profile.Id, Label = profile.Label })
            .ToList();

    public IReadOnlyList<RouteSelectOption> StreamRateControlOptions { get; } =
    [
        new() { Value = "cbr", Label = "CBR (constant bitrate)" },
        new() { Value = "vbr", Label = "VBR (variable bitrate)" }
    ];

    public IReadOnlyList<RouteSelectOption> StreamH264ProfileOptions { get; } =
    [
        new() { Value = "auto", Label = "Auto" },
        new() { Value = "baseline", Label = "Baseline" },
        new() { Value = "main", Label = "Main" },
        new() { Value = "high", Label = "High" }
    ];

    public IReadOnlyList<string> StreamSrtModeOptions { get; } = ["caller", "listener", "rendezvous"];

    public IReadOnlyList<string> StreamSrtKeyLengthOptions { get; } = ["0", "16", "24", "32"];

    public IReadOnlyList<string> SrtIngestModeOptions { get; } = ["listener", "caller", "rendezvous"];

    public string SrtIngestSummary =>
        SrtIngestSources.Count == 1
            ? SrtIngestSources[0].Summary
            : $"{SrtIngestSources.Count}/{MaxSrtIngestSources} SRT sources configured";

    public string SrtIngestCountSummary =>
        $"{SrtIngestSources.Count} of {MaxSrtIngestSources} SRT sources";

    public bool CanAddSrtIngestSource => SrtIngestSources.Count < MaxSrtIngestSources;

    public string SrtIngestRuntimeSummary =>
        "Configured SRT sources are available in Show inputs and Routing.";

    public IReadOnlyList<RouteSelectOption> OutputResolutionOptions { get; } =
    [
        new() { Value = "1280x720", Label = "1280x720 (720p)" },
        new() { Value = "1920x1080", Label = "1920x1080 (1080p)" },
        new() { Value = "2560x1440", Label = "2560x1440 (1440p)" },
        new() { Value = "3840x2160", Label = "3840x2160 (4K)" }
    ];

    public IReadOnlyList<RouteSelectOption> OutputFpsOptions { get; } =
    [
        new() { Value = "24", Label = "24 fps" },
        new() { Value = "30", Label = "30 fps" },
        new() { Value = "50", Label = "50 fps" },
        new() { Value = "60", Label = "60 fps" }
    ];

    public IReadOnlyList<RouteSelectOption> VideoCodecOptions { get; } =
    [
        new() { Value = "h264", Label = "H.264 / AVC" },
        new() { Value = "h265", Label = "H.265 / HEVC" },
        new() { Value = "av1", Label = "AV1" }
    ];

    public IReadOnlyList<RouteSelectOption> StreamEncoderModeOptions { get; } =
    [
        new() { Value = "auto", Label = "Auto" },
        new() { Value = "nvenc", Label = "NVIDIA NVENC" },
        new() { Value = "qsv", Label = "Intel QuickSync" },
        new() { Value = "amf", Label = "AMD AMF" },
        new() { Value = "cpu", Label = "CPU / software" }
    ];

    public IReadOnlyList<RouteSelectOption> RecordingFormatOptions { get; } =
    [
        new() { Value = "mp4", Label = "MP4" },
        new() { Value = "mov", Label = "QuickTime MOV" },
        new() { Value = "mkv", Label = "Matroska MKV" }
    ];

    public IReadOnlyList<RouteSelectOption> RecordingQualityOptions { get; } =
    [
        new() { Value = "high", Label = "High" },
        new() { Value = "standard", Label = "Standard" },
        new() { Value = "archive", Label = "Archive" }
    ];

    public IReadOnlyList<RouteSelectOption> MultiviewLayoutOptions { get; } =
    [
        new() { Value = "grid", Label = "Grid" },
        new() { Value = "pgmPvwTop", Label = "Program/Preview + row" },
        new() { Value = "pgmPvwLarge", Label = "Program/Preview + grid" },
        new() { Value = "pgmPvwSide", Label = "Program/Preview + side strip" }
    ];

    public string CanvasProfileSummary =>
        $"{CanvasResolution} - {NormalizeFpsText(CanvasFps)} fps canvas";

    public string StreamRenderProfileSummary =>
        $"{StreamRenderResolution} - {NormalizeFpsText(StreamRenderFps)} fps - {FormatVideoCodec(StreamVideoCodec)} - {NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps):0.0} Mbps - {FormatStreamEncoderMode(StreamEncoderMode)}";

    public string StreamPlatformProfileSummary =>
        StreamingProfileCatalog.Find(StreamPlatformProfileId).Summary;

    public string StreamBitrateSummary =>
        FormatStreamBitrateSummary(StreamTargetBitrateMbps);

    public string CompactStreamOutputLabel =>
        FormatTransportStreamConfigLabel(StreamTargetBitrateMbps);

    public string RecordingBitrateSummary =>
        FormatStreamBitrateSummary(RecordingTargetBitrateMbps);

    public string CompactRecordingOutputLabel =>
        FormatTransportRecordingConfigLabel(RecordingFormat, RecordingTargetBitrateMbps);

    public string RecordingRenderProfileSummary =>
        $"{RecordingRenderResolution} - {NormalizeFpsText(RecordingRenderFps)} fps - {FormatVideoCodec(RecordingVideoCodec)} - {NormalizeOutputTargetBitrateMbps(RecordingTargetBitrateMbps):0.0} Mbps";

    public string StreamDestinationSummary
    {
        get
        {
            var destinations = BuildSelectedStreamDestinations();
            return destinations.Count == 0
                ? "No stream destinations selected"
                : string.Join(" + ", destinations.Select(destination => destination.ToUpperInvariant()));
        }
    }

    public string StreamConfigurationSummary
    {
        get
        {
            var configured = BuildConfiguredStreamDestinationLabels();
            return configured.Count == 0
                ? $"No stream destinations configured - {StreamRenderProfileSummary}"
                : $"{string.Join(" + ", configured)} - {StreamRenderProfileSummary}";
        }
    }

    public string StreamRtmpSummary =>
        StreamRtmpEnabled
            ? ValidateRtmpSettings() is null
                ? $"{StudioStreamOutputValidation.NormalizeRtmpProtocol(StreamRtmpProtocol).ToUpperInvariant()} ready - {StudioStreamOutputValidation.BuildRtmpUrl(StreamRtmpProtocol, StreamRtmpServerUrl)}"
                : "RTMP/RTMPS needs matching protocol, server URL, and stream key"
            : "RTMP disabled";

    public string StreamNdiSummary =>
        StreamNdiEnabled
            ? _bridge.Profile is not null && !HasNativeOutputCapability(_bridge.Profile, "ndi-output")
                ? "NDI unavailable in this native core build"
                : IsNdiConfigured()
                ? $"NDI ready - {NormalizeOutputText(StreamNdiProgramName, string.Empty)}"
                : "NDI needs a program name"
            : "NDI disabled";

    public string StreamSrtSummary =>
        StreamSrtEnabled
            ? _bridge.Profile is not null && !HasNativeOutputCapability(_bridge.Profile, "srt-output")
                ? "SRT unavailable in this native core build"
                : ValidateSrtSettings() is null
                ? $"SRT ready - {StudioStreamOutputValidation.NormalizeSrtMode(StreamSrtMode)} {NormalizeOutputText(StreamSrtHost, string.Empty)}:{NormalizeOutputText(StreamSrtPort, string.Empty)} - {NormalizeOutputText(StreamSrtLatencyMs, "120")} ms ({BuildSrtEncryptionSummary()})"
                : "SRT needs mode, host, port, latency, and valid encryption settings"
            : "SRT disabled";

    public string RecordingOptionsSummary =>
        $"{NormalizeRecordingFormat(RecordingFormat).ToUpperInvariant()} - {FormatRecordingQuality(RecordingQuality)} - {RecordingRenderProfileSummary} - {RecordingFilenamePrefix}";

    public string OutputStatusBrief => FormatOutputStatusBrief(OutputStatus);

    public string OutputOperatorStatus =>
        ShouldShowOutputStatusDetails(OutputStatus)
            ? $"{OutputStatusBrief}. Open Health for technical details."
            : OutputStatusBrief;

    public string OutputStatusDetailsLabel =>
        ShouldShowOutputStatusDetails(OutputStatus) ? "Show error" : "Details";

    public Visibility OutputStatusDetailsVisibility =>
        ShouldShowOutputStatusDetails(OutputStatus) ? Visibility.Visible : Visibility.Collapsed;

    public Visibility AudioMixerEmptyStateVisibility =>
        AudioParticipantRows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

    public Visibility AudioMixerRowsVisibility =>
        AudioParticipantRows.Count == 0 ? Visibility.Collapsed : Visibility.Visible;

    public Brush RecordButtonBackground => Recording
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 229, 72, 77))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 74, 32, 32));

    public Brush RecordButtonBorder => Recording
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 229, 72, 77))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 224, 90, 90));

    public Brush RecordButtonForeground => Recording
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 248, 248))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 232, 240, 236));

    public Brush StreamButtonBackground => Streaming
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 30, 48, 64));

    public Brush StreamButtonBorder => Streaming
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 90, 159, 212));

    public Brush StreamButtonForeground => Streaming
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 7, 17, 14))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 232, 240, 236));

    public Visibility RecordingLiveDotVisibility => Recording ? Visibility.Visible : Visibility.Collapsed;

    public Visibility RecordIconVisibility => Recording ? Visibility.Collapsed : Visibility.Visible;

    public bool IsStudioTab => ActiveTab == StudioTab.Studio;

    public bool IsSettingsTab => ActiveTab == StudioTab.Settings;

    public bool IsSourcesTab => ActiveTab == StudioTab.Sources;

    public bool IsInputsTab => ActiveTab == StudioTab.Inputs;

    public bool IsRoutingTab => ActiveTab == StudioTab.Routing;

    public bool IsOverlaysTab => ActiveTab == StudioTab.Overlays;

    public bool IsAudioTab => ActiveTab == StudioTab.Audio;

    public bool IsMediaTab => ActiveTab == StudioTab.Media;

    public bool IsAutomationTab => ActiveTab == StudioTab.Automation;

    public string ActiveTabKey => ActiveTab.ToString();

    private static readonly TabChrome SelectedTabChrome = new()
    {
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 26, 61, 46)),
        BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151)),
        Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 232, 240, 236))
    };

    private static readonly TabChrome DefaultTabChrome = new()
    {
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 14, 20, 28)),
        BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 42, 52, 60)),
        Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 184, 200, 192))
    };

    public TabChrome StudioTabChrome => ActiveTab == StudioTab.Studio ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome SettingsTabChrome => ActiveTab == StudioTab.Settings ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome SourcesTabChrome => ActiveTab == StudioTab.Sources ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome InputsTabChrome => ActiveTab == StudioTab.Inputs ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome RoutingTabChrome => ActiveTab == StudioTab.Routing ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome OverlaysTabChrome => ActiveTab == StudioTab.Overlays ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome AudioTabChrome => ActiveTab == StudioTab.Audio ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome MediaTabChrome => ActiveTab == StudioTab.Media ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome AutomationTabChrome => ActiveTab == StudioTab.Automation ? SelectedTabChrome : DefaultTabChrome;

    public Participant? SelectedParticipant =>
        SelectedParticipantId is not null
            ? RoomVideoParticipants.FirstOrDefault(p => p.Id == SelectedParticipantId)
            : null;

    public ParticipantAudioMix? SelectedAudioMix =>
        SelectedParticipantId is not null
            ? AudioMix.Participants.FirstOrDefault(m => m.ParticipantId == SelectedParticipantId)
            : null;

    public string SelectedGainLabel
    {
        get
        {
            var mix = SelectedAudioMix;
            if (mix is null)
            {
                return "0 dB";
            }

            var gain = mix.GainDb;
            return $"{(gain > 0 ? "+" : "")}{gain:0} dB";
        }
    }

    public string SelectedManualGainLabel
    {
        get
        {
            var mix = SelectedAudioMix;
            if (mix is null || mix.ManualGainDb == 0)
            {
                return "Auto";
            }

            return $"{(mix.ManualGainDb > 0 ? "+" : "")}{mix.ManualGainDb:0} dB";
        }
    }

    public string SelectedMuteButtonLabel =>
        SelectedAudioMix?.Muted == true ? "Unmute in mix" : "Mute in mix";

    public int SelectedOutputLevel =>
        SelectedAudioMix?.OutputLevel ?? SelectedParticipant?.AudioLevel ?? 0;

    public string SelectedInsertChainLabel =>
        SelectedAudioMix is { PluginInserts.Count: > 0 } mix
            ? string.Join(" -> ", mix.PluginInserts)
            : "No inserts";

    // ---- C5a: the visible insert rack (LV1-style slot chain). Each applied
    // insert is a SLOT in chain order; built-ins process today (#178),
    // third-party VST3 slots report isolated-host processing or bypass — never
    // faked. Signature-gated rebuild (bound-collection churn is the
    // 0xc000027b crash class).
    private IReadOnlyList<InsertSlotItem> _selectedChannelInsertSlots = [];
    private string _insertSlotsSignature = "";

    public IReadOnlyList<InsertSlotItem> SelectedChannelInsertSlots => _selectedChannelInsertSlots;

    private static readonly string[] BuiltInInsertNames = ["Noise Gate", "Built-in EQ", "Compressor", "Limiter"];

    private static bool IsBuiltInInsert(string name) =>
        !name.StartsWith("VST:", StringComparison.OrdinalIgnoreCase) &&
        (BuiltInInsertNames.Any(builtIn => string.Equals(builtIn, name, StringComparison.OrdinalIgnoreCase)) ||
         name.Contains("gate", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("eq", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("compressor", StringComparison.OrdinalIgnoreCase) ||
         name.Contains("limiter", StringComparison.OrdinalIgnoreCase));

    private static bool IsVstInsert(string name) =>
        name.StartsWith("VST:", StringComparison.OrdinalIgnoreCase) ||
        name.Contains("VST3", StringComparison.OrdinalIgnoreCase);

    private NativeMediaCorePluginHostServe VstServeState =>
        _bridge.LastSnapshot?.AudioMixSession.PluginHost.Serve ?? new();

    private bool IsVstInsertProcessing(string name)
    {
        var serve = VstServeState;
        var query = name.StartsWith("VST:", StringComparison.OrdinalIgnoreCase)
            ? name[4..].Trim()
            : name;
        return serve.Running && serve.StatusCode == 1 && serve.Exchanges > 0 &&
               (string.Equals(serve.ActivePlugin, query, StringComparison.OrdinalIgnoreCase) ||
                query.Contains(serve.ActivePlugin, StringComparison.OrdinalIgnoreCase) ||
                serve.ActivePlugin.Contains(query, StringComparison.OrdinalIgnoreCase));
    }

    private void RefreshSelectedChannelInsertSlots()
    {
        var inserts = SelectedAudioMix?.PluginInserts ?? [];
        var serve = VstServeState;
        var signature = (SelectedParticipantId ?? "") + "|" + string.Join("", inserts) +
                        $"|host:{serve.Running}:{serve.StatusCode}:{serve.ActivePlugin}:{serve.LastError}";
        if (string.Equals(signature, _insertSlotsSignature, StringComparison.Ordinal))
        {
            return;
        }

        _insertSlotsSignature = signature;
        _selectedChannelInsertSlots = inserts
            .Select((name, index) =>
            {
                var builtIn = IsBuiltInInsert(name);
                var processing = !builtIn && IsVstInsertProcessing(name);
                var failed = !builtIn && IsVstInsert(name) && serve.StatusCode == 2 &&
                             !string.IsNullOrWhiteSpace(serve.LastError);
                return new InsertSlotItem
                {
                    // Keep the canonical value: removal/reordering use it as
                    // the insert-chain identity.
                    Name = name,
                    SlotLabel = $"{index + 1}",
                    IsBuiltIn = builtIn,
                    IsProcessing = processing,
                    StatusLabel = builtIn || processing ? "LIVE" : failed ? "BYPASS" : "START",
                    StatusTooltip = builtIn
                        ? "Processing active."
                        : processing
                            ? "Plug-in active. Click to open its controls."
                            : failed
                                ? "Plug-in bypassed; audio continues unchanged. Open Health for details."
                                : "Plug-in starting; audio continues unchanged.",
                };
            })
            .ToList();
        OnPropertyChanged(nameof(SelectedChannelInsertSlots));
    }

    /// <summary>Add-insert options for the rack flyout: built-ins + every scanned VST3.</summary>
    public IReadOnlyList<string> BuiltInInsertOptions => BuiltInInsertNames;

    public void RemoveSelectedChannelInsert(string insertName) => ToggleSelectedRackInsert(insertName);

    // ---- C6 workspace: TwoWay parameter properties over the insertSettings
    // map, one per visible slider. Getters show the core default until the
    // operator moves something; setters ride Set…InsertParam (sync + clamp in
    // the core). Notified by NotifySelectedRackChanged on selection/chain change.
    public double WorkspaceGateThresholdDb
    {
        get => GetSelectedChannelInsertParam("Noise Gate", "thresholdDb", -48);
        set => SetSelectedChannelInsertParam("Noise Gate", "thresholdDb", value, -48);
    }

    public double WorkspaceGateReleaseMs
    {
        get => GetSelectedChannelInsertParam("Noise Gate", "releaseMs", 120);
        set => SetSelectedChannelInsertParam("Noise Gate", "releaseMs", value, 120);
    }

    // C7: competitive gate — attack, hold (stays open between words), and
    // range (ducks to a floor instead of hard silence).
    public double WorkspaceGateAttackMs
    {
        get => GetSelectedChannelInsertParam("Noise Gate", "attackMs", 5);
        set => SetSelectedChannelInsertParam("Noise Gate", "attackMs", value, 5);
    }

    public double WorkspaceGateHoldMs
    {
        get => GetSelectedChannelInsertParam("Noise Gate", "holdMs", 50);
        set => SetSelectedChannelInsertParam("Noise Gate", "holdMs", value, 50);
    }

    public double WorkspaceGateRangeDb
    {
        get => GetSelectedChannelInsertParam("Noise Gate", "rangeDb", -60);
        set => SetSelectedChannelInsertParam("Noise Gate", "rangeDb", value, -60);
    }

    // C7: competitive compressor — envelope times, soft knee, makeup.
    public double WorkspaceCompAttackMs
    {
        get => GetSelectedChannelInsertParam("Compressor", "attackMs", 10);
        set => SetSelectedChannelInsertParam("Compressor", "attackMs", value, 10);
    }

    public double WorkspaceCompReleaseMs
    {
        get => GetSelectedChannelInsertParam("Compressor", "releaseMs", 120);
        set => SetSelectedChannelInsertParam("Compressor", "releaseMs", value, 120);
    }

    public double WorkspaceCompKneeDb
    {
        get => GetSelectedChannelInsertParam("Compressor", "kneeDb", 6);
        set => SetSelectedChannelInsertParam("Compressor", "kneeDb", value, 6);
    }

    public double WorkspaceCompMakeupDb
    {
        get => GetSelectedChannelInsertParam("Compressor", "makeupDb", 0);
        set => SetSelectedChannelInsertParam("Compressor", "makeupDb", value, 0);
    }

    // C7b: live gain-reduction readout for the selected channel. 0..24 dB
    // mapped to a 0..100 meter level; notified per snapshot apply.
    public int WorkspaceCompGrLevel =>
        (int)Math.Clamp((SelectedAudioMix?.GainReductionDb ?? 0) / 24.0 * 100.0, 0, 100);

    public string WorkspaceCompGrLabel =>
        SelectedAudioMix is { GainReductionDb: > 0.05 } mix
            ? $"GR −{mix.GainReductionDb:0.0} dB"
            : "GR —";

    public double WorkspaceEqHighpassHz
    {
        get => GetSelectedChannelInsertParam("Built-in EQ", "highpassHz", 90);
        set => SetSelectedChannelInsertParam("Built-in EQ", "highpassHz", value, 90);
    }

    // C6b: the 8 band gains (63…8k, keys band1Db..band8Db, default 0 = flat).
    private double GetEqBand(int band) =>
        GetSelectedChannelInsertParam("Built-in EQ", $"band{band}Db", 0);

    private void SetEqBand(int band, double value)
    {
        SetSelectedChannelInsertParam("Built-in EQ", $"band{band}Db", value, 0);
        OnPropertyChanged(nameof(WorkspaceEqBandsCsv));
    }

    public double WorkspaceEqBand1 { get => GetEqBand(1); set => SetEqBand(1, value); }
    public double WorkspaceEqBand2 { get => GetEqBand(2); set => SetEqBand(2, value); }
    public double WorkspaceEqBand3 { get => GetEqBand(3); set => SetEqBand(3, value); }
    public double WorkspaceEqBand4 { get => GetEqBand(4); set => SetEqBand(4, value); }
    public double WorkspaceEqBand5 { get => GetEqBand(5); set => SetEqBand(5, value); }
    public double WorkspaceEqBand6 { get => GetEqBand(6); set => SetEqBand(6, value); }
    public double WorkspaceEqBand7 { get => GetEqBand(7); set => SetEqBand(7, value); }
    public double WorkspaceEqBand8 { get => GetEqBand(8); set => SetEqBand(8, value); }

    /// <summary>All 8 band gains as csv for the response-curve control.</summary>
    public string WorkspaceEqBandsCsv =>
        string.Join(",", Enumerable.Range(1, 8).Select(band =>
            GetEqBand(band).ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)));

    public double WorkspaceCompThresholdDb
    {
        get => GetSelectedChannelInsertParam("Compressor", "thresholdDb", -18);
        set => SetSelectedChannelInsertParam("Compressor", "thresholdDb", value, -18);
    }

    public double WorkspaceCompRatio
    {
        get => GetSelectedChannelInsertParam("Compressor", "ratio", 4);
        set => SetSelectedChannelInsertParam("Compressor", "ratio", value, 4);
    }

    private void NotifyWorkspaceParamsChanged()
    {
        OnPropertyChanged(nameof(WorkspaceGateThresholdDb));
        OnPropertyChanged(nameof(WorkspaceGateReleaseMs));
        OnPropertyChanged(nameof(WorkspaceGateAttackMs));
        OnPropertyChanged(nameof(WorkspaceGateHoldMs));
        OnPropertyChanged(nameof(WorkspaceGateRangeDb));
        OnPropertyChanged(nameof(WorkspaceCompAttackMs));
        OnPropertyChanged(nameof(WorkspaceCompReleaseMs));
        OnPropertyChanged(nameof(WorkspaceCompKneeDb));
        OnPropertyChanged(nameof(WorkspaceCompMakeupDb));
        OnPropertyChanged(nameof(WorkspaceEqHighpassHz));
        for (var band = 1; band <= 8; band++)
        {
            OnPropertyChanged($"WorkspaceEqBand{band}");
        }

        OnPropertyChanged(nameof(WorkspaceEqBandsCsv));
        OnPropertyChanged(nameof(WorkspaceCompThresholdDb));
        OnPropertyChanged(nameof(WorkspaceCompRatio));
    }

    // C5d: the rack header name must work for EVERY channel — local audio and
    // media are audio rows, not Zoom participants, so SelectedParticipant.Name
    // showed "—" while a chain was clearly loaded (owner screenshot).
    public string SelectedChannelDisplayName =>
        AudioParticipantRows.FirstOrDefault(row =>
            string.Equals(row.Id, SelectedParticipantId, StringComparison.Ordinal))?.Name
        ?? SelectedParticipant?.Name
        ?? "Select a channel";

    // ---- C5b: built-in insert parameters. Missing key = the core's default;
    // the slider flyout reads via Get (showing the default) and writes via Set,
    // which syncs the whole insertSettings map on the mixer wire.
    public double GetSelectedChannelInsertParam(string insertName, string paramKey, double fallback) =>
        SelectedAudioMix?.InsertSettings.TryGetValue(insertName, out var parameters) == true &&
        parameters.TryGetValue(paramKey, out var value)
            ? value
            : fallback;

    public void SetSelectedChannelInsertParam(string insertName, string paramKey, double value, double? defaultValue = null)
    {
        if (SelectedAudioMix is not { } mix)
        {
            return;
        }

        if (!mix.InsertSettings.TryGetValue(insertName, out var parameters))
        {
            parameters = new Dictionary<string, double>(StringComparer.OrdinalIgnoreCase);
            mix.InsertSettings[insertName] = parameters;
        }

        // No-op guard. CRITICAL blind spot fixed: when the key is NOT stored
        // yet, the getter had returned the DEFAULT and TwoWay bindings write
        // that default straight back on every notify - without comparing
        // against the default, enabling an insert cascaded ~20 writes, each
        // notifying all workspace params and syncing the core (the gate-
        // enable burst). A missing key equals its default.
        var current = parameters.TryGetValue(paramKey, out var stored) ? stored : defaultValue;
        if (current is { } comparable && Math.Abs(comparable - value) < 0.001)
        {
            return;
        }

        parameters[paramKey] = value;
        CommandStatus = $"{insertName}: {paramKey} = {value:0.#}";
        // C7c (owner: "the UI is still not reflected when you make changes"):
        // the response curves bind the Workspace* properties OneWay — without
        // this notify a slider drag never re-drew its curve.
        NotifyWorkspaceParamsChanged();
        _ = TrySyncMediaCoreAsync();
    }

    public void AddVstInsertToSelectedChannel(string pluginClassName)
    {
        if (SelectedAudioMix is not { } mix || string.IsNullOrWhiteSpace(pluginClassName))
        {
            return;
        }

        var insertName = pluginClassName.StartsWith("VST:", StringComparison.OrdinalIgnoreCase)
            ? pluginClassName.Trim()
            : $"VST:{pluginClassName.Trim()}";

        if (mix.PluginInserts.Any(insert => string.Equals(insert, insertName, StringComparison.OrdinalIgnoreCase)))
        {
            CommandStatus = $"{pluginClassName} is already on {SelectedParticipant?.Name ?? "selected channel"}";
            return;
        }

        mix.PluginInserts.Add(insertName);
        CommandStatus = $"{pluginClassName} added to {SelectedParticipant?.Name ?? "selected channel"}.";
        RefreshMixerValueBindings(mix.ParticipantId);
        RefreshAudioProcessingTargets();
        NotifySelectedRackChanged();
        _ = TrySyncMediaCoreAsync();
    }

    public string VstBridgeStatusLabel =>
        SelectedAudioMix?.PluginInserts.Any(IsVstInsert) == true
            ? FormatVstServeStatus(VstServeState)
            : "No VST3 insert on selected channel";

    public async Task OpenVstControlsAsync(string insertName)
    {
        if (!IsVstInsert(insertName))
        {
            CommandStatus = "Select a VST3 insert to open its controls.";
            return;
        }

        var label = insertName.StartsWith("VST:", StringComparison.OrdinalIgnoreCase)
            ? insertName[4..].Trim()
            : insertName;
        CommandStatus = $"Opening {label} controls…";
        try
        {
            await _bridge.OpenVstEditorAsync(insertName);
        }
        catch (Exception ex)
        {
            CommandStatus = $"Could not open plug-in controls: {ex.Message}";
        }
    }

    public Task OpenSelectedAudioProcessingVstControlsAsync()
    {
        var insert = ResolveAudioProcessingInserts(
                NormalizeAudioProcessingTargetId(SelectedAudioProcessingTargetId))
            .FirstOrDefault(IsVstInsert);
        if (string.IsNullOrWhiteSpace(insert))
        {
            CommandStatus = "Add a VST3 plug-in to this processing target first.";
            return Task.CompletedTask;
        }

        return OpenVstControlsAsync(insert);
    }

    public void RemoveAudioProcessingInsert(string insertName)
    {
        var targetId = NormalizeAudioProcessingTargetId(SelectedAudioProcessingTargetId);
        var inserts = ResolveAudioProcessingInsertList(targetId);
        var removed = inserts.RemoveAll(candidate =>
            string.Equals(candidate, insertName, StringComparison.OrdinalIgnoreCase));
        if (removed == 0) return;

        CommandStatus = $"Removed {insertName} from {ResolveAudioProcessingTargetName(targetId)}";
        RefreshAudioProcessingTargets();
        RefreshSelectedAudioProcessingSlots();
        RefreshAudioParticipantRows();
        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
    }

    public AudioProcessingTargetOption? SelectedAudioProcessingTarget =>
        AudioProcessingTargetOptions.FirstOrDefault(target =>
            string.Equals(target.Id, SelectedAudioProcessingTargetId, StringComparison.Ordinal));

    public string SelectedAudioProcessingTargetLabel =>
        SelectedAudioProcessingTarget?.Label ?? "No processing target selected";

    public string SelectedAudioProcessingTargetKindLabel =>
        SelectedAudioProcessingTarget?.Kind ?? "Target";

    public string SelectedAudioProcessingTargetDetail =>
        SelectedAudioProcessingTarget?.Detail ?? "Choose a channel, bus, aux, or master path before adding processing.";

    public string SelectedAudioProcessingInsertLabel =>
        SelectedAudioProcessingTarget?.InsertLabel ?? "No inserts";

    private IReadOnlyList<InsertSlotItem> _selectedAudioProcessingSlots = [];
    private string _audioProcessingSlotsSignature = string.Empty;

    public IReadOnlyList<InsertSlotItem> SelectedAudioProcessingSlots => _selectedAudioProcessingSlots;

    private void RefreshSelectedAudioProcessingSlots()
    {
        var inserts = ResolveAudioProcessingInserts(NormalizeAudioProcessingTargetId(SelectedAudioProcessingTargetId));
        var serve = VstServeState;
        var signature = (SelectedAudioProcessingTargetId ?? string.Empty) + "|" + string.Join("\u0001", inserts) +
                        $"|host:{serve.Running}:{serve.StatusCode}:{serve.ActivePlugin}:{serve.LastError}:" +
                        $"{serve.EditorStatusCode}:{serve.EditorActivePlugin}:{serve.EditorLastError}";
        if (string.Equals(signature, _audioProcessingSlotsSignature, StringComparison.Ordinal)) return;

        _audioProcessingSlotsSignature = signature;
        _selectedAudioProcessingSlots = inserts.Select((name, index) =>
        {
            var builtIn = IsBuiltInInsert(name);
            var processing = !builtIn && IsVstInsertProcessing(name);
            var failed = !builtIn && IsVstInsert(name) && serve.StatusCode == 2 &&
                         !string.IsNullOrWhiteSpace(serve.LastError);
            return new InsertSlotItem
            {
                Name = name,
                SlotLabel = $"{index + 1:00}",
                IsBuiltIn = builtIn,
                IsProcessing = processing,
                StatusLabel = builtIn || processing ? "LIVE" : failed ? "BYPASS" : "START",
                StatusTooltip = builtIn
                    ? "Processing active."
                    : processing
                        ? "Plug-in active. Click to open its controls."
                        : failed
                            ? "Plug-in bypassed; audio continues unchanged. Open Health for details."
                            : "Plug-in starting; audio continues unchanged. Click to open its controls."
            };
        }).ToList();
        OnPropertyChanged(nameof(SelectedAudioProcessingSlots));
    }

    public string ProcessingBridgeStatusLabel =>
        ResolveAudioProcessingInserts(NormalizeAudioProcessingTargetId(SelectedAudioProcessingTargetId)).Any(IsVstInsert)
            ? FormatVstServeStatus(VstServeState)
            : "Built-in processing settings are synced with the native media core metadata.";

    private static string FormatVstServeStatus(NativeMediaCorePluginHostServe serve) =>
        serve.Running && serve.StatusCode == 1 && serve.Exchanges > 0
            ? $"Processing {serve.ActivePlugin} in the isolated VST3 host."
            : serve.StatusCode == 2 && !string.IsNullOrWhiteSpace(serve.LastError)
                ? $"Bypassed safely: {serve.LastError}"
                : serve.DeadlineMisses > 0
                    ? $"Host starting or missed its deadline ({serve.DeadlineMisses}); audio remains bypassed."
                    : "Isolated VST3 host starting; audio remains bypassed until the first processed block returns.";

    // Status-pill design language: a pill is GREEN (dot + tint + text) when its
    // thing is on/live, RED when off/offline. Capture and Zoom share this so the
    // operator reads state by colour + dot, not by wording. Green #22C86E, red
    // #E5433F (StudioAccent / StudioAir).
    private static SolidColorBrush PillBackground(bool on) => new(on
        ? Windows.UI.Color.FromArgb(31, 34, 200, 110)
        : Windows.UI.Color.FromArgb(28, 229, 67, 63));
    private static SolidColorBrush PillBorder(bool on) => new(on
        ? Windows.UI.Color.FromArgb(150, 34, 200, 110)
        : Windows.UI.Color.FromArgb(150, 229, 67, 63));
    private static SolidColorBrush PillForeground(bool on) => new(on
        ? Windows.UI.Color.FromArgb(255, 174, 242, 223)
        : Windows.UI.Color.FromArgb(255, 247, 176, 174));
    private static SolidColorBrush PillDot(bool on) => new(on
        ? Windows.UI.Color.FromArgb(255, 34, 200, 110)
        : Windows.UI.Color.FromArgb(255, 229, 67, 63));

    public Brush EngineToggleBackground => PillBackground(ZoomCaptureSubscribed);
    public Brush EngineToggleBorder => PillBorder(ZoomCaptureSubscribed);
    public Brush EngineToggleForeground => PillForeground(ZoomCaptureSubscribed);
    public Brush CaptureDotBrush => PillDot(ZoomCaptureSubscribed);

    // Zoom connection pill: just "Zoom", red until a meeting is live then green.
    public bool ZoomOnline => ZoomStatus == "Zoom Live";
    public Brush ZoomPillBackground => PillBackground(ZoomOnline);
    public Brush ZoomPillBorder => PillBorder(ZoomOnline);
    public Brush ZoomPillForeground => PillForeground(ZoomOnline);
    public Brush ZoomPillDot => PillDot(ZoomOnline);

    public IReadOnlyList<ParticipantListItem> ParticipantListItems { get; private set; } = [];

    private string _programResolutionLabel = "1080p60";
    public string ProgramResolutionLabel
    {
        get => _programResolutionLabel;
        private set => SetProperty(ref _programResolutionLabel, value);
    }

    private string ProgramFrameResolutionLabel =>
        ProgramSurface.LastFrame is { Width: > 0, Height: > 0 } frame
            ? $"{frame.Width}×{frame.Height} {frame.Fps:0.#}"
            : "1920×1080 60";

    public string CurrentRoomHeader => $"Video in room ({RoomVideoParticipants.Count})";

    public string ViewModeLabel => ViewMode switch
    {
        StudioViewMode.Multiview => "MULTIVIEW",
        StudioViewMode.Preview => "PREVIEW",
        StudioViewMode.ProgramPreview => "PROGRAM + PREVIEW",
        _ => "PROGRAM"
    };

    public bool ShowProgram => ViewMode is StudioViewMode.Program or StudioViewMode.ProgramPreview;

    public bool ShowPreview => ViewMode is StudioViewMode.Preview or StudioViewMode.ProgramPreview;

    public bool ShowProgramPreviewSplit => ViewMode == StudioViewMode.ProgramPreview;

    public bool ShowProgramSolo => ShowProgram && !ShowPreview;

    public bool ShowPreviewSolo => ShowPreview && !ShowProgram;

    public bool ShowProgramInSplit => ShowProgram && ShowPreview;

    public bool ShowMultiview => ViewMode == StudioViewMode.Multiview;

    public bool ShowMultiviewStrip => ViewMode != StudioViewMode.Multiview;

    public string MultiviewHeader
    {
        get
        {
            var displayed = MultiviewGridTiles.Count(tile => !tile.IsEmpty);
            var inputLabel = displayed == 1 ? "input" : "inputs";
            return $"{displayed} live {inputLabel}";
        }
    }

    public string ShowInputSummary
    {
        get
        {
            var assigned = ShowInputs.Count(slot => slot.IsAssigned);
            var inShow = ShowInputs.Count(slot => slot.InShow && slot.IsAssigned);
            return $"{inShow} in show · {assigned} assigned";
        }
    }

    public string SetupProgressSummary
    {
        get
        {
            var inShow = ShowInputs.Count(slot => slot.InShow && slot.IsAssigned);
            var inputLabel = inShow == 1 ? "input" : "inputs";
            var guestLabel = RoomVideoParticipants.Count == 1 ? "guest" : "guests";
            return $"{inShow} {inputLabel} · {RoomVideoParticipants.Count} Zoom {guestLabel}";
        }
    }

    public string ShowStatusSummary
    {
        get
        {
            var assigned = ShowInputs.Count(slot => slot.IsAssigned);
            var inShow = ShowInputs.Count(slot => slot.InShow && slot.IsAssigned);
            if (assigned == 0)
            {
                return "Add sources";
            }

            if (inShow == 0)
            {
                return "Choose inputs for the show";
            }

            return Recording || Streaming ? "On air" : "Ready";
        }
    }

    public string ShowReadinessSummary
    {
        get
        {
            var assigned = ShowInputs.Count(slot => slot.IsAssigned);
            var inShow = ShowInputs.Count(slot => slot.InShow && slot.IsAssigned);
            if (!Settings.IsInMeeting)
            {
                return "Start by joining Zoom, then assign the live guests to Inputs 1-10.";
            }

            if (assigned == 0)
            {
                return "Zoom is live. Next: open Sources and assign guests, media, or a camera to Inputs 1-10.";
            }

            if (inShow == 0)
            {
                return "Inputs are assigned. Toggle In show for the sources you want on the multiview and in scenes.";
            }

            if (!Recording && !Streaming)
            {
                return "Ready to produce. Pick a scene, Take it to Program, then start Record or Stream.";
            }

            return $"Live output active. {OutputStatus}";
        }
    }

    public string MultiviewConfigureHint =>
        "Build scenes in Sources · assign show inputs for multiview";

    public string SceneBuilderHint =>
        "Open the Scenes tab to drag sources on the 16:9 canvas";

    public int PreviewSlotCount => PreviewCanvasLayers.Count;

    public bool HasPreviewSlotEditors => PreviewCanvasLayers.Count > 0;

    public string SceneBuilderSlotSummary =>
        PreviewCanvasLayers.Count == 0
            ? "No sources on canvas — pick a scene on Studio first"
            : $"{PreviewCanvasLayers.Count} sources on canvas for {PreviewScene.Name}";

    public string MultiviewCapLabel =>
        $"UP TO {ShowInputRosterService.MaxMultiviewBoxes} LIVE";

    private static readonly TabChrome SelectedViewModeChrome = new()
    {
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 26, 61, 46)),
        BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 61, 220, 151)),
        Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 232, 240, 236))
    };

    private static readonly TabChrome DefaultViewModeChrome = new()
    {
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 12, 18, 24)),
        BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 36, 46, 54)),
        Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 184, 200, 192))
    };

    public TabChrome ProgramModeChrome =>
        ViewMode == StudioViewMode.Program ? SelectedViewModeChrome : DefaultViewModeChrome;

    public TabChrome PreviewModeChrome =>
        ViewMode == StudioViewMode.Preview ? SelectedViewModeChrome : DefaultViewModeChrome;

    public TabChrome SplitModeChrome =>
        ViewMode == StudioViewMode.ProgramPreview ? SelectedViewModeChrome : DefaultViewModeChrome;

    public TabChrome MultiviewModeChrome =>
        ViewMode == StudioViewMode.Multiview ? SelectedViewModeChrome : DefaultViewModeChrome;

    partial void OnViewModeChanged(StudioViewMode value)
    {
        OnPropertyChanged(nameof(ViewModeLabel));
        OnPropertyChanged(nameof(ShowProgram));
        OnPropertyChanged(nameof(ShowPreview));
        OnPropertyChanged(nameof(ShowProgramSolo));
        OnPropertyChanged(nameof(ShowPreviewSolo));
        OnPropertyChanged(nameof(ShowProgramInSplit));
        OnPropertyChanged(nameof(ShowMultiview));
        OnPropertyChanged(nameof(ShowMultiviewStrip));
        OnPropertyChanged(nameof(ShowProgramPreviewSplit));
        OnPropertyChanged(nameof(ProgramModeChrome));
        OnPropertyChanged(nameof(PreviewModeChrome));
        OnPropertyChanged(nameof(SplitModeChrome));
        OnPropertyChanged(nameof(MultiviewModeChrome));
    }

    partial void OnMultiviewTilesChanged(IReadOnlyList<ParticipantSurfaceTile> value)
    {
        OnPropertyChanged(nameof(MultiviewHeader));
    }

    partial void OnMultiviewGridTilesChanged(IReadOnlyList<ParticipantSurfaceTile> value)
    {
        OnPropertyChanged(nameof(MultiviewHeader));
    }

    // Multiviewer preferences: layout mode + tile count + overlay toggles. On any change, clamp,
    // persist, and (debounced) push a configure-multiviewer command to the core.
    // The Studio center IS the unified multiviewer, so it defaults to a Program/Preview
    // layout (PGM + PVW cells on top over the source-feed tiles) rather than a bare source grid,
    // so it ALWAYS shows Preview/Program plus the enabled sources.
    public const string DefaultMultiviewLayoutMode = "pgmPvwTop";
    // Tile capacity matches the 10 Show Input slots (ShowInputRosterService.MaxMultiviewBoxes),
    // so every enabled input can be on the wall at once.
    public const int DefaultMultiviewTileCount = 10;
    public const int MinMultiviewTileCount = 1;
    public const int MaxMultiviewTileCount = 10;

    public static int ClampMultiviewTileCount(int value) =>
        Math.Clamp(value, MinMultiviewTileCount, MaxMultiviewTileCount);

    // Const-only body (no static-field access) so this stays callable from the headless test host
    // without triggering the StudioViewModel type initializer (which needs the WinUI runtime).
    public static string NormalizeMultiviewLayoutMode(string? value) => value switch
    {
        "grid" or "pgmPvwTop" or "pgmPvwLarge" or "pgmPvwSide" => value,
        _ => DefaultMultiviewLayoutMode
    };

    partial void OnMultiviewLayoutModeChanged(string value)
    {
        var normalized = NormalizeMultiviewLayoutMode(value);
        if (!string.Equals(value, normalized, StringComparison.Ordinal))
        {
            MultiviewLayoutMode = normalized;
            return;
        }

        OnMultiviewerConfigChanged();
    }

    partial void OnMultiviewTileCountChanged(int value)
    {
        var clamped = ClampMultiviewTileCount(value);
        if (clamped != value)
        {
            MultiviewTileCount = clamped;
            return;
        }

        OnMultiviewerConfigChanged();
    }

    partial void OnMultiviewShowLabelsChanged(bool value) => OnMultiviewerConfigChanged();

    partial void OnMultiviewShowTallyChanged(bool value) => OnMultiviewerConfigChanged();

    partial void OnMultiviewShowMetersChanged(bool value) => OnMultiviewerConfigChanged();

    partial void OnMultiviewShowClockChanged(bool value) => OnMultiviewerConfigChanged();

    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _multiviewerConfigTimer;

    private void OnMultiviewerConfigChanged()
    {
        OnPropertyChanged(nameof(MultiviewerConfigSummary));
        SaveProductionOutputPreferences();
        ScheduleMultiviewerConfigResend();
    }

    // Debounce rapid toggling (e.g. dragging the tile-count spinner) into a single send. Also
    // the backpressure-retry hook: ConfigureMultiviewerAsync re-arms this when a send is dropped
    // because a production sync was in flight, so the operator's saved layout eventually reaches
    // the core instead of being silently lost.
    private void ScheduleMultiviewerConfigResend()
    {
        if (_multiviewerConfigTimer is null)
        {
            _multiviewerConfigTimer = _dispatcher.CreateTimer();
            _multiviewerConfigTimer.IsRepeating = false;
            _multiviewerConfigTimer.Tick += (_, _) => _ = ConfigureMultiviewerAsync();
        }

        _multiviewerConfigTimer.Interval = TimeSpan.FromMilliseconds(150);
        _multiviewerConfigTimer.Start();
    }

    public string MultiviewerConfigSummary
    {
        get
        {
            var layoutLabel = MultiviewLayoutMode switch
            {
                "pgmPvwTop" => "Program/Preview + row",
                "pgmPvwLarge" => "Program/Preview + grid",
                "pgmPvwSide" => "Program/Preview + side strip",
                _ => "Grid"
            };
            return $"{layoutLabel} - {MultiviewTileCount} tiles";
        }
    }

    private async Task ConfigureMultiviewerAsync()
    {
        if (!_bridge.Running)
        {
            return;
        }

        var command = MediaCoreCommandBuilder.BuildConfigureMultiviewerCommand(
            NormalizeMultiviewLayoutMode(MultiviewLayoutMode),
            ClampMultiviewTileCount(MultiviewTileCount),
            MultiviewShowLabels,
            MultiviewShowTally,
            MultiviewShowMeters,
            MultiviewShowClock);

        try
        {
            // Standalone sync — does not apply the response snapshot, so it cannot re-trigger the
            // production-sync path.
            await _bridge.SyncAsync([command]).ConfigureAwait(false);
        }
        catch (MediaCoreSyncInFlightException)
        {
            // A production sync was already in flight (common during the startup burst), so this
            // command was dropped for backpressure. Re-arm the debounce to retry once the in-flight
            // sync clears — otherwise the operator's saved multiviewer layout silently never reaches
            // the core (observed: layout stayed "grid" despite a persisted pgmPvw choice).
            RunOnUiThread(ScheduleMultiviewerConfigResend);
        }
        catch
        {
            // Other transient errors: the next change / startup re-apply retries.
        }
    }

    partial void OnPrimaryCaptureDeviceIdChanged(string? value) => ApplyDualCaptureSelection();

    partial void OnSecondaryCaptureDeviceIdChanged(string? value) => ApplyDualCaptureSelection();

    partial void OnZoomCaptureSubscribedChanged(bool value)
    {
        ToggleEngineCommand.NotifyCanExecuteChanged();
        ToggleRecordingCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(CanToggleRecording));
        OnPropertyChanged(nameof(EngineRunningLabel));
        OnPropertyChanged(nameof(EngineToggleBackground));
        OnPropertyChanged(nameof(EngineToggleBorder));
        OnPropertyChanged(nameof(EngineToggleForeground));
        OnPropertyChanged(nameof(CaptureDotBrush));
        if (_bridge.LastSnapshot is { } snapshot)
        {
            ApplyMeetingFieldsFromSnapshot(snapshot);
        }

        OnPropertyChanged(nameof(CurrentRoomHeader));
        RefreshTransportState();
    }

    partial void OnZoomStatusChanged(string value)
    {
        OnPropertyChanged(nameof(ZoomOnline));
        OnPropertyChanged(nameof(ZoomPillBackground));
        OnPropertyChanged(nameof(ZoomPillBorder));
        OnPropertyChanged(nameof(ZoomPillForeground));
        OnPropertyChanged(nameof(ZoomPillDot));
    }

    partial void OnRecordingChanged(bool value)
    {
        OnPropertyChanged(nameof(RecordingLabel));
        OnPropertyChanged(nameof(RecordButtonBackground));
        OnPropertyChanged(nameof(RecordButtonBorder));
        OnPropertyChanged(nameof(RecordButtonForeground));
        OnPropertyChanged(nameof(RecordingLiveDotVisibility));
        OnPropertyChanged(nameof(RecordIconVisibility));
        OnPropertyChanged(nameof(ShowStatusSummary));
        RefreshTransportState();
    }

    partial void OnStreamingChanged(bool value)
    {
        OnPropertyChanged(nameof(StreamingLabel));
        OnPropertyChanged(nameof(StreamButtonBackground));
        OnPropertyChanged(nameof(StreamButtonBorder));
        OnPropertyChanged(nameof(StreamButtonForeground));
        OnPropertyChanged(nameof(ShowStatusSummary));
        RefreshTransportState();
    }

    partial void OnMasterLimiterEnabledChanged(bool value)
    {
        if (_suppressMasteringSync)
        {
            return;
        }

        RefreshAudioReadoutBindings();
        RefreshTransportState();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnMasteringEnabledChanged(bool value)
    {
        if (_suppressMasteringSync)
        {
            return;
        }

        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnMasteringTargetIndexChanged(int value) => SyncMasteringControlChange();

    partial void OnMasteringGlueAmountChanged(double value) => SyncMasteringControlChange();

    partial void OnMasteringCeilingDbfsChanged(double value) => SyncMasteringControlChange();

    partial void OnMasteringMaxRideDbChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringInputGainDbChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringHighPassHzChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringLowPassHzChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringLowShelfDbChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringPresenceDbChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringHighShelfDbChanged(double value) => SyncMasteringControlChange();
    partial void OnMasteringStereoWidthChanged(double value) => SyncMasteringControlChange();

    private void SyncMasteringControlChange()
    {
        if (_suppressMasteringSync)
        {
            return;
        }

        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private void SelectMasteringCompareSlot(string? slotId)
    {
        var normalized = string.Equals(slotId, "B", StringComparison.OrdinalIgnoreCase) ? "B" : "A";
        if (string.Equals(normalized, _masteringCompareSlot, StringComparison.Ordinal))
        {
            // A checked ToggleButton can still receive a click. Re-assert the
            // one-way state so comparison always has exactly one active slot.
            OnPropertyChanged(normalized == "A" ? nameof(IsMasteringCompareA) : nameof(IsMasteringCompareB));
            return;
        }

        if (IsMasteringCompareA)
        {
            _masteringCompareA = CaptureMasteringSettings();
        }
        else
        {
            _masteringCompareB = CaptureMasteringSettings();
        }

        _masteringCompareSlot = normalized;
        OnPropertyChanged(nameof(MasteringCompareSlot));
        OnPropertyChanged(nameof(IsMasteringCompareA));
        OnPropertyChanged(nameof(IsMasteringCompareB));
        ApplyMasteringSettings(IsMasteringCompareA ? _masteringCompareA : _masteringCompareB);
        CommandStatus = $"Master processor recalled comparison {_masteringCompareSlot}.";
    }

    [RelayCommand]
    private void ApplyMasteringPreset(string? presetId)
    {
        if (string.IsNullOrWhiteSpace(presetId))
        {
            return;
        }

        var settings = MasteringPresetCatalog.Create(presetId);
        ApplyMasteringSettings(settings);
        CommandStatus = $"{CultureInfo.InvariantCulture.TextInfo.ToTitleCase(presetId)} mastering starting point applied to comparison {_masteringCompareSlot}.";
    }

    [RelayCommand]
    private void ResetMasteringStage(string? stageId)
    {
        if (string.IsNullOrWhiteSpace(stageId))
        {
            return;
        }

        ApplyMasteringSettings(MasteringPresetCatalog.ResetStage(CaptureMasteringSettings(), stageId));
        CommandStatus = $"{CultureInfo.InvariantCulture.TextInfo.ToTitleCase(stageId)} stage reset to neutral.";
    }

    private MasteringSettings CaptureMasteringSettings() => new(
        MasteringEnabled,
        MasteringTargetIndex,
        MasteringGlueAmount,
        MasteringCeilingDbfs,
        MasteringMaxRideDb,
        MasteringInputGainDb,
        MasteringHighPassHz,
        MasteringLowPassHz,
        MasteringLowShelfDb,
        MasteringPresenceDb,
        MasteringHighShelfDb,
        MasteringStereoWidth,
        MasterLimiterEnabled);

    private void ApplyMasteringSettings(MasteringSettings settings)
    {
        _suppressMasteringSync = true;
        try
        {
            MasteringEnabled = settings.Enabled;
            MasteringTargetIndex = settings.TargetIndex;
            MasteringGlueAmount = settings.GlueAmount;
            MasteringCeilingDbfs = settings.CeilingDbfs;
            MasteringMaxRideDb = settings.MaxRideDb;
            MasteringInputGainDb = settings.InputGainDb;
            MasteringHighPassHz = settings.HighPassHz;
            MasteringLowPassHz = settings.LowPassHz;
            MasteringLowShelfDb = settings.LowShelfDb;
            MasteringPresenceDb = settings.PresenceDb;
            MasteringHighShelfDb = settings.HighShelfDb;
            MasteringStereoWidth = settings.StereoWidth;
            MasterLimiterEnabled = settings.LimiterEnabled;
        }
        finally
        {
            _suppressMasteringSync = false;
        }

        if (IsMasteringCompareA)
        {
            _masteringCompareA = settings;
        }
        else
        {
            _masteringCompareB = settings;
        }

        RefreshAudioReadoutBindings();
        RefreshTransportState();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnAudioMonitoringEnabledChanged(bool value)
    {
        if (value && AudioMonitorVolume <= 0)
        {
            AudioMonitorVolume = DefaultAudioMonitorVolume;
            return;
        }

        OnAudioMonitorSettingsChanged();
    }

    partial void OnSelectedAudioMonitorDeviceIdChanged(string value) => OnAudioMonitorSettingsChanged();

    partial void OnAudioMonitorVolumeChanged(double value) => OnAudioMonitorSettingsChanged();

    private bool _applyingLowerThirdTimingPreset;

    partial void OnSelectedLowerThirdTimingPresetChanged(string value)
    {
        if (string.Equals(value, "Custom", StringComparison.Ordinal)) return;
        var timing = value switch
        {
            "Smooth" => (BuildIn: 420d, BuildOut: 320d),
            "Gentle" => (BuildIn: 700d, BuildOut: 550d),
            _ => (BuildIn: 220d, BuildOut: 160d)
        };
        _applyingLowerThirdTimingPreset = true;
        LowerThirdBuildInMs = timing.BuildIn;
        LowerThirdBuildOutMs = timing.BuildOut;
        _applyingLowerThirdTimingPreset = false;
        ApplyProgramLowerThirdTimingChange();
    }

    partial void OnLowerThirdBuildInMsChanged(double value)
    {
        var normalized = NormalizeLowerThirdTimingMs(value);
        if (!value.Equals(normalized))
        {
            LowerThirdBuildInMs = normalized;
            return;
        }

        if (!_applyingLowerThirdTimingPreset)
        {
            SelectedLowerThirdTimingPreset = "Custom";
            ApplyProgramLowerThirdTimingChange();
        }
    }

    partial void OnLowerThirdBuildOutMsChanged(double value)
    {
        var normalized = NormalizeLowerThirdTimingMs(value);
        if (!value.Equals(normalized))
        {
            LowerThirdBuildOutMs = normalized;
            return;
        }

        if (!_applyingLowerThirdTimingPreset)
        {
            SelectedLowerThirdTimingPreset = "Custom";
            ApplyProgramLowerThirdTimingChange();
        }
    }

    private void ApplyProgramLowerThirdTimingChange()
    {
        OnPropertyChanged(nameof(LowerThirdTimingSummary));
        OnPropertyChanged(nameof(StudioLowerThirdToolTip));
        SaveProductionOutputPreferences();

        if (!ProgramLowerThirdKey.IsVisible)
        {
            return;
        }

        ProgramLowerThirdKey = ProgramLowerThirdKey with
        {
            BuildInMs = NormalizeLowerThirdTimingMs(LowerThirdBuildInMs),
            BuildOutMs = NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs)
        };
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnProgramLowerThirdEnabledChanged(bool value)
    {
        RefreshProgramLowerThirdKeyPosition();
        OnPropertyChanged(nameof(StudioLowerThirdButtonLabel));
        OnPropertyChanged(nameof(StudioLowerThirdCompactLabel));
        OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
        OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
        OnPropertyChanged(nameof(StudioLowerThirdToolTip));
    }

    partial void OnPreviewSceneBackgroundAssetIdChanged(string value)
    {
        if (_refreshingSceneBackgroundSelection)
        {
            return;
        }

        var selection = SceneBackgroundSelectionService.ApplySelection(
            PreviewSceneId,
            value,
            _sceneBackgroundAssetIds,
            FindMediaAsset,
            IsVisualMediaAsset);
        SaveProductionOutputPreferences();
        CommandStatus = selection.HasBackground
            ? $"{selection.AssetName ?? selection.SelectedAssetId} set as SuperSource background for {PreviewScene.Name}"
            : $"SuperSource background cleared for {PreviewScene.Name}";

        NotifySceneBackgroundChanged();
        SchedulePreviewRoutingRefresh();
        if (SceneBackgroundSelectionService.SelectionAffectsProgramScene(PreviewSceneId, ActiveSceneId))
        {
            _ = TrySyncMediaCoreAsync();
        }
    }

    partial void OnLocalAudioSourceEnabledChanged(bool value)
    {
        if (value)
        {
            var resolvedLocalDeviceId = ResolveLocalAudioSourceDeviceId(SelectedLocalAudioCaptureDeviceId, AudioCaptureDevices);
            if (!string.Equals(resolvedLocalDeviceId, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal))
            {
                SelectedLocalAudioCaptureDeviceId = resolvedLocalDeviceId;
                return;
            }
        }

        BuildAudioRoutingMatrix();
        RefreshAudioParticipantRows();
        RefreshLocalAudioSourceBindings();
        SaveProductionOutputPreferences();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnSelectedLocalAudioCaptureDeviceIdChanged(string value)
    {
        BuildAudioRoutingMatrix();
        RefreshAudioParticipantRows();
        RefreshLocalAudioSourceBindings();
        SaveProductionOutputPreferences();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnSelectedAudioProcessingTargetIdChanged(string value)
    {
        OnPropertyChanged(nameof(SelectedAudioProcessingTarget));
        OnPropertyChanged(nameof(SelectedAudioProcessingTargetLabel));
        OnPropertyChanged(nameof(SelectedAudioProcessingTargetKindLabel));
        OnPropertyChanged(nameof(SelectedAudioProcessingTargetDetail));
        OnPropertyChanged(nameof(SelectedAudioProcessingInsertLabel));
        OnPropertyChanged(nameof(ProcessingBridgeStatusLabel));
        RefreshSelectedAudioProcessingSlots();
    }

    partial void OnFfmpegBinDirectoryChanged(string value)
    {
        ApplyFfmpegRuntimeEnvironment(value);
        OnPropertyChanged(nameof(FfmpegRuntimeStatus));
        OnPropertyChanged(nameof(FfmpegOperatorStatus));
        SaveProductionOutputPreferences();
    }

    partial void OnStreamRtmpEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamNdiEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamRtmpProtocolChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnStreamOutputOptionChanged();
    }

    partial void OnStreamRtmpServerUrlChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamRtmpStreamKeyChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamNdiProgramNameChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamNdiGroupNameChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtModeChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtHostChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtPortChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtLatencyMsChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtStreamIdChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtKeyLengthChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtPassphraseChanged(string value) => OnStreamOutputOptionChanged();

    partial void OnOutputStatusChanged(string value)
    {
        OnPropertyChanged(nameof(OutputStatusBrief));
        OnPropertyChanged(nameof(OutputOperatorStatus));
        OnPropertyChanged(nameof(OutputStatusDetailsLabel));
        OnPropertyChanged(nameof(OutputStatusDetailsVisibility));
    }

    partial void OnCanvasResolutionChanged(string value) => OnOutputProfileChanged();

    partial void OnCanvasFpsChanged(string value) => OnOutputProfileChanged();

    partial void OnStreamRenderResolutionChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamRenderFpsChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamVideoCodecChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamTargetBitrateMbpsChanged(double value)
    {
        var normalized = NormalizeStreamTargetBitrateMbps(value);
        if (!value.Equals(normalized))
        {
            StreamTargetBitrateMbps = normalized;
            return;
        }

        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamAudioBitrateKbpsChanged(double value)
    {
        var normalized = NormalizeAudioBitrateKbps(value);
        if (!value.Equals(normalized))
        {
            StreamAudioBitrateKbps = normalized;
            return;
        }

        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamEncoderModeChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamPlatformProfileIdChanged(string value)
    {
        OnPropertyChanged(nameof(StreamPlatformProfileSummary));
        var profile = StreamingProfileCatalog.Find(value);
        if (_applyingStreamingProfile || profile.Id == StreamingProfileCatalog.CustomProfileId)
        {
            SaveProductionOutputPreferences();
            return;
        }

        _applyingStreamingProfile = true;
        try
        {
            StreamRenderResolution = profile.Resolution;
            StreamRenderFps = profile.Fps;
            StreamVideoCodec = profile.VideoCodec;
            StreamTargetBitrateMbps = profile.TargetBitrateMbps;
            StreamAudioBitrateKbps = profile.AudioBitrateKbps;
            StreamKeyframeIntervalSeconds = profile.KeyframeIntervalSeconds;
            StreamRateControl = profile.RateControl;
            StreamH264Profile = profile.H264Profile;
            StreamBFrames = profile.BFrames;
            StreamAllowEnhancedRtmp = profile.AllowEnhancedRtmp;
        }
        finally
        {
            _applyingStreamingProfile = false;
        }

        OnOutputProfileChanged();
    }

    partial void OnStreamKeyframeIntervalSecondsChanged(double value)
    {
        var normalized = Math.Round(Math.Clamp(value, 0.5, 10), 1);
        if (!value.Equals(normalized))
        {
            StreamKeyframeIntervalSeconds = normalized;
            return;
        }
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamRateControlChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamH264ProfileChanged(string value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamBFramesChanged(double value)
    {
        var normalized = Math.Round(Math.Clamp(value, 0, 4));
        if (!value.Equals(normalized))
        {
            StreamBFrames = normalized;
            return;
        }
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnStreamAllowEnhancedRtmpChanged(bool value)
    {
        if (_applyingStreamingProfile) return;
        MarkStreamingProfileCustom();
        OnOutputProfileChanged();
    }

    partial void OnRecordingRenderResolutionChanged(string value) => OnOutputProfileChanged();

    partial void OnRecordingRenderFpsChanged(string value) => OnOutputProfileChanged();

    partial void OnRecordingVideoCodecChanged(string value) => OnOutputProfileChanged();

    partial void OnRecordingTargetBitrateMbpsChanged(double value)
    {
        var normalized = NormalizeOutputTargetBitrateMbps(value);
        if (!value.Equals(normalized))
        {
            RecordingTargetBitrateMbps = normalized;
            return;
        }

        OnOutputProfileChanged();
    }

    partial void OnRecordingAudioBitrateKbpsChanged(double value)
    {
        var normalized = NormalizeAudioBitrateKbps(value);
        if (!value.Equals(normalized))
        {
            RecordingAudioBitrateKbps = normalized;
            return;
        }

        OnOutputProfileChanged();
    }

    partial void OnRecordingTargetFolderChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnRecordingFilenamePrefixChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnRecordingFormatChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnRecordingQualityChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnProductionModeChanged(ProductionMode value)
    {
        RefreshTransportAutomationState();
        EvaluateAutomationPolicy();
        OnPropertyChanged(nameof(AutomationButtonLabel));
        OnPropertyChanged(nameof(AutoProductionReadout));
    }

    partial void OnActiveTabChanged(StudioTab value)
    {
        OnPropertyChanged(nameof(IsStudioTab));
        OnPropertyChanged(nameof(IsSettingsTab));
        OnPropertyChanged(nameof(IsSourcesTab));
        OnPropertyChanged(nameof(IsInputsTab));
        OnPropertyChanged(nameof(IsRoutingTab));
        OnPropertyChanged(nameof(IsOverlaysTab));
        OnPropertyChanged(nameof(IsAudioTab));
        OnPropertyChanged(nameof(IsMediaTab));
        OnPropertyChanged(nameof(IsAutomationTab));
        OnPropertyChanged(nameof(ActiveTabKey));
        OnPropertyChanged(nameof(StudioTabChrome));
        OnPropertyChanged(nameof(SettingsTabChrome));
        OnPropertyChanged(nameof(SourcesTabChrome));
        OnPropertyChanged(nameof(InputsTabChrome));
        OnPropertyChanged(nameof(RoutingTabChrome));
        OnPropertyChanged(nameof(OverlaysTabChrome));
        OnPropertyChanged(nameof(AudioTabChrome));
        OnPropertyChanged(nameof(MediaTabChrome));
        OnPropertyChanged(nameof(AutomationTabChrome));

        if (value == StudioTab.Inputs)
        {
            _ = RefreshCaptureDevicesAsync();
        }
        else if (value == StudioTab.Routing)
        {
            BuildAudioRoutingMatrix();
            BuildVideoRoutingMatrix();
        }
        else if (value == StudioTab.Media)
        {
            RefreshMediaBin();
        }
    }

    partial void OnActiveSceneIdChanged(string value)
    {
        RefreshSceneItems();
        OnPropertyChanged(nameof(ProgramScene));
        OnPropertyChanged(nameof(ProgramSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
        OnPropertyChanged(nameof(CanTake));
        TakeCommand.NotifyCanExecuteChanged();
        SchedulePreviewRoutingRefresh();
    }

    partial void OnPreviewSceneIdChanged(string value)
    {
        // S2b: cueing a different scene abandons any uncommitted edits to the
        // live scene (the draft belongs to the previously cued scene).
        DiscardLivePreviewDraft();
        SceneBuilderName = PreviewScene.Name;
        RefreshSceneItems();
        RefreshSceneBackgroundSelection();
        OnPropertyChanged(nameof(PreviewScene));
        OnPropertyChanged(nameof(PreviewSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
        OnPropertyChanged(nameof(CanTake));
        TakeCommand.NotifyCanExecuteChanged();
        SchedulePreviewRoutingRefresh();
        // Push the newly-selected PREVIEW scene graph to the core so it composites the
        // multi-layer preview bus (mirrors how program scene changes sync). Discrete user
        // action, so a single sync — no flood. Backpressure is transient (the periodic sync
        // reapplies), so swallow the in-flight signal.
        if (_bridge.Running)
        {
            _ = SyncPreviewSceneChangeAsync();
        }
    }

    private async Task SyncPreviewSceneChangeAsync()
    {
        try
        {
            await SyncActiveSceneAsync("preview-scene-change").ConfigureAwait(false);
        }
        catch (MediaCoreSyncInFlightException)
        {
            // Another sync was already running; the preview scene will be applied by the
            // next sync (or the continuous periodic sync). Not a failure.
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"preview-scene sync failed: {ex.Message}");
        }
    }

    partial void OnSelectedParticipantIdChanged(string? value)
    {
        _surfaces.SetPreviewParticipant(value);
        RefreshParticipantListItems();
        OnPropertyChanged(nameof(SelectedParticipant));
        OnPropertyChanged(nameof(SelectedAudioMix));
        OnPropertyChanged(nameof(SelectedGainLabel));
        OnPropertyChanged(nameof(SelectedManualGainLabel));
        OnPropertyChanged(nameof(SelectedMuteButtonLabel));
        OnPropertyChanged(nameof(SelectedOutputLevel));
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        OnPropertyChanged(nameof(VstBridgeStatusLabel));
        NotifySelectedRackChanged();  // C2 rack follows the selected channel
        RefreshAudioParticipantSelectionRows();
    }

    partial void OnProgramSurfaceChanged(VideoSurfaceState value)
    {
        OnPropertyChanged(nameof(ProgramResolutionLabel));
        RefreshTransportState();
    }

    partial void OnProgramLowerThirdKeyChanged(LowerThirdKeyState value)
    {
        OnPropertyChanged(nameof(LowerThirdKeyStatus));
        OnPropertyChanged(nameof(LowerThirdKeySummary));
        OnPropertyChanged(nameof(StudioLowerThirdButtonLabel));
        OnPropertyChanged(nameof(StudioLowerThirdCompactLabel));
        OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
        OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
        OnPropertyChanged(nameof(StudioLowerThirdToolTip));
    }

    partial void OnAutomationAutoTakeEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationPreferScreenShareChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationLowerThirdsEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationAutoAssignInputsEnabledChanged(bool value)
    {
        OnAutomationPolicyChanged();
        // Turning it on should immediately fill free slots from the current roster.
        ReapplyShowInputAutoAssign();
    }

    partial void OnAutomationCaptionsEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnTakeTransitionModeChanged(string value)
    {
        var normalized = NormalizeTakeTransitionMode(value);
        if (!string.Equals(value, normalized, StringComparison.Ordinal))
        {
            TakeTransitionMode = normalized;
            return;
        }

        OnPropertyChanged(nameof(TakeTransitionLabel));
        OnPropertyChanged(nameof(TakeToolTip));
    }

    partial void OnAutomationConfidenceThresholdChanged(double value)
    {
        var clamped = Math.Clamp(value, 0, 100);
        if (Math.Abs(clamped - value) > 0.01)
        {
            AutomationConfidenceThreshold = clamped;
            return;
        }

        OnAutomationPolicyChanged();
    }

    partial void OnAutomationSwitchDelaySecondsChanged(double value)
    {
        var clamped = Math.Clamp(value, 0, 30);
        if (Math.Abs(clamped - value) > 0.01)
        {
            AutomationSwitchDelaySeconds = clamped;
            return;
        }

        OnAutomationPolicyChanged();
    }

    partial void OnAutomationPanelParticipantThresholdChanged(double value)
    {
        var clamped = Math.Clamp(value, 2, 10);
        if (Math.Abs(clamped - value) > 0.01)
        {
            AutomationPanelParticipantThreshold = clamped;
            return;
        }

        OnAutomationPolicyChanged();
    }

    private void OnAutomationPolicyChanged()
    {
        _automationPendingSceneId = null;
        _automationPendingSince = null;
        OnPropertyChanged(nameof(AutomationPolicySummary));
        OnPropertyChanged(nameof(AutomationScenePolicySummary));
        OnPropertyChanged(nameof(AutomationOverlayPolicySummary));
        OnPropertyChanged(nameof(AutomationTakeModeLabel));
        RefreshProgramLowerThirdKeyPosition();
        RefreshProductionReadouts();
    }

    private static string NormalizeTakeTransitionMode(string? transitionMode) =>
        transitionMode?.Trim().ToLowerInvariant() switch
        {
            "cut" => "cut",
            "dip" => "dip",
            "wipe" => "wipe",
            _ => "fade"
        };

    [RelayCommand]
    private void ResetAutomationDefaults()
    {
        AutomationAutoTakeEnabled = true;
        AutomationPreferScreenShare = true;
        AutomationLowerThirdsEnabled = true;
        AutomationAutoAssignInputsEnabled = true;
        AutomationCaptionsEnabled = true;
        AutomationConfidenceThreshold = 70;
        AutomationSwitchDelaySeconds = 4;
        AutomationPanelParticipantThreshold = 4;
        AutomationLastAction = "Automation defaults restored";
        CommandStatus = "Automation defaults restored";
    }

    // The top-right toggle controls the Zoom CAPTURE SUBSCRIPTION, not the media core.
    // The media core / compositor is always on (started on join, stopped on leave) so
    // the program/preview surfaces always show the live compositor output. Toggling here
    // only subscribes/unsubscribes the raw Zoom ingest spine sync.
    [RelayCommand(CanExecute = nameof(CanToggleCapture))]
    private async Task ToggleEngineAsync()
    {
        try
        {
            if (ZoomCaptureSubscribed)
            {
                UnsubscribeZoomCapture("Zoom capture paused");
            }
            else
            {
                // The media core should already be running from launch. If it died, bring it back up rather than dead-ending.
                // ConfigureAwait(true): this is a UI [RelayCommand]; the whole
                // continuation below sets x:Bound properties (EngineStatus,
                // ZoomCaptureSubscribed) + RefreshSurfaceBindings, so it MUST resume on
                // the UI thread or it fail-fasts (CoreMessagingXP 0xc000027b). This was
                // the recurring Engine-On crash.
                await EnsureMediaCoreRunningAsync("Restarting media core...").ConfigureAwait(true);

                if (!_bridge.Running)
                {
                    EngineStatus = "Media core is unavailable — rejoin the meeting.";
                    return;
                }

                EngineStatus = "Requesting Zoom capture…";
                _bridge.ConfigureZoomSpineSync(BuildSpinePayload);
                ZoomCaptureSubscribed = true;
                _surfaces.SetZoomCaptureSubscribed(true, _bridge.Profile?.Renderer);
                _surfaces.SetPreviewParticipant(SelectedParticipantId);
                EngineStatus = $"Capture live — {_bridge.ProfileSummary}";
                Settings.RefreshSdkReadiness();
                try
                {
                    await SyncActiveSceneAsync().ConfigureAwait(true);
                }
                catch (MediaCoreSyncInFlightException)
                {
                    // Transient backpressure: another sync was already running when
                    // the operator flipped Engine On. Capture is enabled and the
                    // spine/periodic sync will apply the active scene shortly — this
                    // is NOT a toggle failure, so keep capture on.
                    EngineStatus = $"Capture live — {_bridge.ProfileSummary}";
                }
                RefreshSurfaceBindings();
                RefreshTransportState();
                ToggleRecordingCommand.NotifyCanExecuteChanged();
            }
        }
        catch (MediaCoreSyncInFlightException)
        {
            // Backpressure during toggle — non-fatal; leave capture enabled.
            EngineStatus = "Capture starting…";
            RefreshTransportState();
            ToggleRecordingCommand.NotifyCanExecuteChanged();
        }
        catch (Exception ex)
        {
            ZoomCaptureSubscribed = false;
            _surfaces.SetZoomCaptureSubscribed(false);
            EngineStatus = ex.Message;
            Settings.RefreshSdkReadiness();
            RefreshSurfaceBindings();
            RefreshTransportState();
            ToggleRecordingCommand.NotifyCanExecuteChanged();
        }
    }

    [RelayCommand]
    private void SelectScene(string sceneId)
    {
        PreviewSceneId = sceneId;
        CommandStatus = $"{Scenes.First(s => s.Id == sceneId).Name} queued on preview";
        SchedulePreviewRoutingRefresh();
    }

    [RelayCommand]
    private void RemoveScene(string sceneId)
    {
        if (!CanRemoveScene(sceneId))
        {
            CommandStatus = "Only idle custom scenes can be removed";
            return;
        }

        var scene = _scenes.FirstOrDefault(item => string.Equals(item.Id, sceneId, StringComparison.Ordinal));
        if (scene is null)
        {
            return;
        }

        if (string.Equals(PreviewSceneId, sceneId, StringComparison.Ordinal))
        {
            PreviewSceneId = ActiveSceneId;
        }

        _scenes.Remove(scene);
        _sceneRoutes.Remove(sceneId);
        CommandStatus = $"{scene.Name} removed";
        RefreshSceneItems();
        RefreshProductionReadouts();
        SaveProductionOutputPreferences();
    }

    [RelayCommand]
    private void PreviewMultiviewTile(ParticipantSurfaceTile? tile)
    {
        if (tile is not { IsEmpty: false })
        {
            return;
        }

        var sceneId = ActiveSceneId == MultiviewSoloSceneAId
            ? MultiviewSoloSceneBId
            : MultiviewSoloSceneAId;
        var scene = EnsureMultiviewSoloScene(sceneId);
        var routes = GetMutableRoutes(scene.Id);
        routes.Clear();
        routes.Add(BuildSoloRoute(scene.Id, tile));

        var previousPreviewSceneId = PreviewSceneId;
        PreviewSceneId = scene.Id;
        if (string.Equals(previousPreviewSceneId, scene.Id, StringComparison.Ordinal) && _bridge.Running)
        {
            // Re-cue into the SAME solo scene (program isn't on a solo scene, so every click
            // reuses one id): only the ROUTES changed, the PreviewSceneId setter no-oped, and
            // the core sync fires only on a scene-id change — so push the route change
            // explicitly or the PREVIEW bus keeps compositing the previous source.
            _ = SyncPreviewSceneChangeAsync();
        }

        OnPropertyChanged(nameof(PreviewSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
        CommandStatus = $"{tile.Participant.Name} queued as manual one-up preview";
        SchedulePreviewRoutingRefresh();
    }

    // Multiviewer drag-drop reorder: dragging one source tile onto another swaps the two Show
    // Input SLOT ASSIGNMENTS (the wall order IS the slot order), then the normal show-input
    // change path re-syncs the core layout, persistence, and the overlay tile rects.
    [RelayCommand]
    private void SwapMultiviewTiles(MultiviewTileSwapRequest? request)
    {
        if (request is null || request.FromSlot == request.ToSlot)
        {
            return;
        }

        var from = ShowInputs.FirstOrDefault(slot => slot.SlotNumber == request.FromSlot);
        var to = ShowInputs.FirstOrDefault(slot => slot.SlotNumber == request.ToSlot);
        if (from is null || to is null)
        {
            return;
        }

        ShowInputRosterService.SwapSlotAssignments(from, to);
        // One line per operator gesture — cheap, and invaluable when reconstructing a show.
        LaunchLog.Write($"mv-drag: swapped slot{request.FromSlot}<->slot{request.ToSlot} " +
            $"now slot{from.SlotNumber}={from.Kind}/{from.CaptureDeviceId}{from.ParticipantId} slot{to.SlotNumber}={to.Kind}/{to.CaptureDeviceId}{to.ParticipantId}");
        CommandStatus = $"Inputs {request.FromSlot} and {request.ToSlot} swapped on the multiviewer.";
    }

    public bool CanTake => PreviewSceneId != ActiveSceneId;

    public string TakeTransitionLabel => TakeTransitionMode switch
    {
        "cut" => "Cut",
        "dip" => "Dip",
        "wipe" => "Wipe",
        _ => "Fade"
    };

    public string TakeToolTip =>
        $"Take Preview to Program using {TakeTransitionLabel}, then swap the previous Program back to Preview.";

    private static string NewCustomSceneId() =>
        $"custom-{Guid.NewGuid():N}".Substring(0, "custom-".Length + 8);

    private Scene EnsureMultiviewSoloScene(string sceneId)
    {
        if (Scenes.FirstOrDefault(scene => scene.Id == sceneId) is { } existing)
        {
            return existing;
        }

        var scene = new Scene
        {
            Id = sceneId,
            Name = "Manual one-up",
            Layout = ManualOneUpLayout,
            Automation = "Manual source selection"
        };

        _scenes.Add(scene);
        _sceneRoutes[scene.Id] = [];
        RefreshSceneItems();
        return scene;
    }

    private static SourceRoute BuildSoloRoute(string sceneId, ParticipantSurfaceTile tile)
    {
        var participantId = tile.Participant.Id;
        var isCaptureDevice = participantId.StartsWith("capture:", StringComparison.Ordinal);
        return new SourceRoute
        {
            Id = $"{sceneId}-1",
            Mode = isCaptureDevice ? SourceRouteMode.CaptureDevice : SourceRouteMode.Fixed,
            ParticipantId = isCaptureDevice ? null : participantId,
            CaptureDeviceId = isCaptureDevice ? participantId["capture:".Length..] : null,
            AudioRole = SourceAudioRole.Isolated,
            CanvasRect = new NormalizedCanvasRect
            {
                X = 0,
                Y = 0,
                Width = 1,
                Height = 1
            },
            ZIndex = 0
        };
    }

    [RelayCommand]
    private void NewScene()
    {
        var newId = NewCustomSceneId();
        var sceneNumber = _scenes.Count(s => s.Id.StartsWith("custom-", StringComparison.Ordinal)) + 1;
        var scene = new Scene
        {
            Id = newId,
            Name = $"New scene {sceneNumber}",
            Layout = "host-focus",
            Automation = "Custom canvas"
        };

        _scenes.Add(scene);
        _sceneRoutes[newId] = SceneRoutingService
            .GetRouteDefaults(scene, existingRoutes: null, RoomVideoParticipants)
            .Select(route => route.Clone())
            .ToList();

        RefreshSceneItems();
        PreviewSceneId = newId;
        CommandStatus = $"{scene.Name} created and queued on preview";
        SchedulePreviewRoutingRefresh();
        SaveProductionOutputPreferences();
    }

    [RelayCommand]
    private void DuplicateScene(string? sceneId)
    {
        var source = _scenes.FirstOrDefault(scene => string.Equals(scene.Id, sceneId, StringComparison.Ordinal));
        if (source is null)
        {
            return;
        }

        var newId = NewCustomSceneId();
        var scene = new Scene
        {
            Id = newId,
            Name = ScenePersistenceService.MakeUniqueSceneName($"{source.Name} copy", _scenes.Select(s => s.Name)),
            Layout = source.Layout,
            Automation = "Custom canvas"
        };

        _scenes.Add(scene);
        _sceneRoutes[newId] = GetMutableRoutes(source.Id)
            .Select(route => route.Clone())
            .ToList();

        RefreshSceneItems();
        PreviewSceneId = newId;
        CommandStatus = $"{scene.Name} duplicated from {source.Name} and cued on preview";
        SchedulePreviewRoutingRefresh();
        SaveProductionOutputPreferences();
    }

    [RelayCommand]
    private void SaveScene(string? name)
    {
        var trimmed = string.IsNullOrWhiteSpace(name) ? null : name.Trim();
        if (trimmed is null)
        {
            UpdateScene(PreviewScene.Name);
            return;
        }

        var existing = _scenes.FirstOrDefault(
            s => trimmed is not null && string.Equals(s.Name, trimmed, StringComparison.OrdinalIgnoreCase));

        if (existing is not null)
        {
            // No silent clobber (scenes redesign S2): Save used to overwrite a
            // same-named scene's routes without warning. Overwriting is the
            // explicit Update action.
            CommandStatus = $"{existing.Name} already exists - use Update scene to overwrite it, or choose a new name";
            return;
        }

        var sceneNumber = _scenes.Count(s => s.Id.StartsWith("custom-", StringComparison.Ordinal)) + 1;
        var newId = NewCustomSceneId();
        var scene = new Scene
        {
            Id = newId,
            Name = trimmed ?? $"Saved scene {sceneNumber}",
            Layout = PreviewScene.Layout,
            Automation = "Custom canvas"
        };

        _scenes.Add(scene);
        _sceneRoutes[newId] = GetPreviewEditableRoutes()
            .Select(route => route.Clone())
            .ToList();

        RefreshSceneItems();
        PreviewSceneId = newId;
        CommandStatus = $"{scene.Name} saved from canvas";
        SchedulePreviewRoutingRefresh();
        SaveProductionOutputPreferences();
    }

    [RelayCommand]
    private void UpdateScene(string? name)
    {
        var scene = PreviewScene;
        var trimmed = string.IsNullOrWhiteSpace(name) ? scene.Name : name.Trim();
        var updatesProgramScene = string.Equals(scene.Id, ActiveSceneId, StringComparison.Ordinal);
        var previousProgramMediaRoutes = updatesProgramScene
            ? BuildProgramMediaRouteSignature(GetMutableRoutes(scene.Id))
            : string.Empty;
        var duplicate = _scenes.FirstOrDefault(item =>
            !string.Equals(item.Id, scene.Id, StringComparison.Ordinal) &&
            string.Equals(item.Name, trimmed, StringComparison.OrdinalIgnoreCase));

        if (duplicate is not null)
        {
            CommandStatus = $"{duplicate.Name} already exists - choose a unique scene name";
            SceneBuilderName = scene.Name;
            return;
        }

        CopyPreviewRoutesToScene(scene.Id);
        if (updatesProgramScene &&
            !string.Equals(previousProgramMediaRoutes, BuildProgramMediaRouteSignature(GetMutableRoutes(scene.Id)), StringComparison.Ordinal))
        {
            _programMediaPlaybackTakeVersion++;
            PromoteProgramMediaRouteToPlayback();
        }

        if (!string.Equals(scene.Name, trimmed, StringComparison.Ordinal))
        {
            RenameScene(scene.Id, trimmed);
        }

        SceneBuilderName = trimmed;
        CommandStatus = $"{trimmed} updated";
        RefreshSceneItems();
        RefreshProductionReadouts();
        SchedulePreviewRoutingRefresh();
        SaveProductionOutputPreferences();
        if (updatesProgramScene)
        {
            OutputStatus = "Program updated";
            _ = TrySyncMediaCoreAsync();
        }
    }

    private void RenameScene(string sceneId, string name)
    {
        var index = _scenes.ToList().FindIndex(scene => string.Equals(scene.Id, sceneId, StringComparison.Ordinal));
        if (index < 0)
        {
            return;
        }

        var scene = _scenes[index];
        _scenes[index] = new Scene
        {
            Id = scene.Id,
            Name = name,
            Layout = scene.Layout,
            Automation = scene.Automation,
            DurationLabel = scene.DurationLabel
        };

        OnPropertyChanged(nameof(ProgramScene));
        OnPropertyChanged(nameof(PreviewScene));
        OnPropertyChanged(nameof(ProgramSceneSummary));
        OnPropertyChanged(nameof(PreviewSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
    }

    // Take promotes Preview → Program. Because the compositor is always on, the program
    // composition + native scene sync must run regardless of whether Zoom capture is
    // subscribed; otherwise the program feed never reflects the new scene.
    [RelayCommand(CanExecute = nameof(CanTake))]
    private async Task TakeAsync()
    {
        var previousProgramSceneId = ActiveSceneId;
        var takenSceneId = PreviewSceneId;

        ActiveSceneId = takenSceneId;
        PreviewSceneId = previousProgramSceneId;
        _programMediaPlaybackTakeVersion++;
        PromoteProgramMediaRouteToPlayback();
        RefreshPreviewRoutingState();
        CommandStatus = $"{ProgramSceneSummary} taken with {TakeTransitionLabel.ToLowerInvariant()}";
        OutputStatus = "Program updated";

        if (_bridge.Running)
        {
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                RunOnUiThread(() => CommandStatus = ex.Message);  // catch runs off-thread (ConfigureAwait(false))
            }
        }
    }

    [RelayCommand]
    private void SetTakeTransition(string? transitionMode)
    {
        TakeTransitionMode = NormalizeTakeTransitionMode(transitionMode);
        CommandStatus = $"Take transition set to {TakeTransitionLabel}";
    }

    [RelayCommand(CanExecute = nameof(CanToggleRecording))]
    private async Task ToggleRecordingAsync()
    {
        if (_recordingToggleInFlight)
        {
            return;
        }

        _recordingToggleInFlight = true;
        ToggleRecordingCommand.NotifyCanExecuteChanged();
        var starting = !Recording;
        var previousRecording = Recording;
        LaunchLog.Write(
            $"recording: toggle requested action={(starting ? "start" : "stop")} " +
            $"format={NormalizeRecordingFormat(RecordingFormat)} " +
            $"bitrate={NormalizeOutputTargetBitrateMbps(RecordingTargetBitrateMbps):0.0}Mbps " +
            $"codec={RecordingVideoCodec}");

        Recording = starting;
        RefreshOutputStatus();

        try
        {
            await EnsureMediaCoreRunningAsync(starting ? "Starting media core for recording..." : "Updating media core...").ConfigureAwait(false);
            var snapshot = await SyncActiveSceneAsync().ConfigureAwait(false);
            if (starting && TryFormatRecordingStartHealthFailure(snapshot, out var healthFailureStatus))
            {
                RunOnUiThread(() =>
                {
                    Recording = previousRecording;
                    RefreshOutputStatus();
                    OutputStatus = healthFailureStatus;
                    OutputSessionStatus = OutputStatus;
                });
                _ = SyncFailedRecordingStartRollbackAsync(healthFailureStatus);
                return;
            }

            RunOnUiThread(() =>
            {
                OutputStatus = starting ? "Recording start requested." : "Recording stopped.";
                OutputSessionStatus = OutputStatus;
            });
        }
        catch (MediaCoreSyncInFlightException)
        {
            RunOnUiThread(() =>
            {
                OutputStatus = starting ? "Recording starting - applying..." : "Recording stopping - applying...";
                OutputSessionStatus = OutputStatus;
            });
            LaunchLog.Write($"recording: {(starting ? "start" : "stop")} deferred because media core sync is in flight");
            _ = RetryRecordingSyncAsync(starting);
        }
        catch (Exception ex)
        {
            var action = starting ? "start" : "stop";
            var failureStatus = FormatRecordingFailureStatus(action, ex);
            LaunchLog.Write($"recording: {action} failed {ex.GetType().Name}: {ex.Message}");
            RunOnUiThread(() =>
            {
                Recording = previousRecording;
                RefreshOutputStatus();
                OutputStatus = failureStatus;
                OutputSessionStatus = OutputStatus;
            });

            if (starting && !previousRecording)
            {
                _ = SyncFailedRecordingStartRollbackAsync(failureStatus);
            }
        }
        finally
        {
            RunOnUiThread(() =>
            {
                _recordingToggleInFlight = false;
                ToggleRecordingCommand.NotifyCanExecuteChanged();
            });
        }
    }

    private async Task SyncFailedRecordingStartRollbackAsync(string failureStatus)
    {
        for (var attempt = 0; attempt < 4; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Still busy; retry shortly.
            }
            catch (Exception ex)
            {
                var rollbackFailure = NormalizeStreamingFailureDetail(ex.Message);
                RunOnUiThread(() =>
                {
                    OutputStatus = $"{failureStatus} Recording cleanup also failed: {rollbackFailure}";
                    OutputSessionStatus = OutputStatus;
                });
                return;
            }
        }

        RunOnUiThread(() =>
        {
            OutputStatus = $"{failureStatus} Recording cleanup is still waiting for media core.";
            OutputSessionStatus = OutputStatus;
        });
    }

    private async Task RetryRecordingSyncAsync(bool starting)
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                var snapshot = await SyncActiveSceneAsync().ConfigureAwait(false);
                if (starting && TryFormatRecordingStartHealthFailure(snapshot, out var healthFailureStatus))
                {
                    RunOnUiThread(() =>
                    {
                        Recording = ResolveRecordingStateAfterFailedRetry(starting);
                        RefreshOutputStatus();
                        OutputStatus = healthFailureStatus;
                        OutputSessionStatus = OutputStatus;
                    });
                    _ = SyncFailedRecordingStartRollbackAsync(healthFailureStatus);
                    return;
                }

                RunOnUiThread(() =>
                {
                    OutputStatus = starting ? "Recording start requested." : "Recording stopped.";
                    OutputSessionStatus = OutputStatus;
                    RefreshOutputStatus();
                });
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Still busy; try again shortly.
            }
            catch (Exception ex)
            {
                var failureStatus = FormatRecordingFailureStatus(starting ? "start" : "stop", ex);
                RunOnUiThread(() =>
                {
                    Recording = ResolveRecordingStateAfterFailedRetry(starting);
                    RefreshOutputStatus();
                    OutputStatus = failureStatus;
                    OutputSessionStatus = OutputStatus;
                });

                if (starting)
                {
                    _ = SyncFailedRecordingStartRollbackAsync(failureStatus);
                }
                return;
            }
        }

        var exhaustedStatus = FormatRecordingSyncRetryExhaustedStatus(starting);
        RunOnUiThread(() =>
        {
            Recording = ResolveRecordingStateAfterFailedRetry(starting);
            RefreshOutputStatus();
            OutputStatus = exhaustedStatus;
            OutputSessionStatus = OutputStatus;
        });

        if (starting)
        {
            _ = SyncFailedRecordingStartRollbackAsync(exhaustedStatus);
        }
    }

    private bool CanToggleStreaming() => !_streamToggleInFlight;

    [RelayCommand(CanExecute = nameof(CanToggleStreaming))]
    private async Task ToggleStreamingAsync()
    {
        if (_streamToggleInFlight)
        {
            return;
        }

        _streamToggleInFlight = true;
        ToggleStreamingCommand.NotifyCanExecuteChanged();
        var starting = !Streaming;
        var requestedDestinations = BuildSelectedStreamDestinations(validatedOnly: true);
        LaunchLog.Write(
            $"stream: toggle requested action={(starting ? "start" : "stop")} " +
            $"rtmp={StreamRtmpEnabled} ndi={StreamNdiEnabled} srt={StreamSrtEnabled} " +
            $"destinations={FormatStreamDestinationTelemetry(requestedDestinations)} " +
            $"bitrate={NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps):0.0}Mbps " +
            $"codec={StreamVideoCodec} encoder={StreamEncoderMode}");
        try
        {
            if (starting && ValidateStreamDestinations() is { Length: > 0 } validationError)
            {
                var failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(validationError));
                LaunchLog.Write($"stream: start blocked ({validationError})");
                OutputStatus = failureStatus;
                OutputSessionStatus = failureStatus;
                return;
            }

            var previousStreaming = Streaming;
            Streaming = starting;
            RefreshOutputStatus();

            try
            {
                await EnsureMediaCoreRunningAsync(starting ? "Starting media core for stream..." : "Updating media core...").ConfigureAwait(false);
                if (starting && ValidateStreamDestinations() is { Length: > 0 } runtimeValidationError)
                {
                    var failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(runtimeValidationError));
                    LaunchLog.Write($"stream: start blocked after media-core profile ({runtimeValidationError})");
                    RunOnUiThread(() =>
                    {
                        Streaming = previousStreaming;
                        RefreshOutputStatus();
                        OutputStatus = failureStatus;
                        OutputSessionStatus = OutputStatus;
                    });
                    return;
                }

                var snapshot = await SyncActiveSceneAsync(starting ? "stream-start" : "stream-stop").ConfigureAwait(false);
                if (starting && TryFormatStreamingStartHealthFailure(snapshot, out var healthFailureStatus))
                {
                    LaunchLog.Write($"stream: start failed health proof {healthFailureStatus}");
                    RunOnUiThread(() =>
                    {
                        Streaming = previousStreaming;
                        RefreshOutputStatus();
                        OutputStatus = healthFailureStatus;
                        OutputSessionStatus = OutputStatus;
                    });
                    _ = SyncFailedStreamStartRollbackAsync(healthFailureStatus);
                    return;
                }

                if (starting)
                {
                    var proof = await WaitForStreamingStartProofAsync(requestedDestinations, snapshot).ConfigureAwait(false);
                    if (!string.IsNullOrWhiteSpace(proof.FailureStatus))
                    {
                        LaunchLog.Write($"stream: start failed sender proof {proof.FailureStatus}");
                        RunOnUiThread(() =>
                        {
                            Streaming = previousStreaming;
                            RefreshOutputStatus();
                            OutputStatus = proof.FailureStatus;
                            OutputSessionStatus = OutputStatus;
                        });
                        _ = SyncFailedStreamStartRollbackAsync(proof.FailureStatus);
                        return;
                    }
                }

                RunOnUiThread(() =>
                {
                    OutputStatus = starting ? "Streaming start requested." : "Streaming stopped.";
                    OutputSessionStatus = OutputStatus;
                });
            }
            catch (MediaCoreSyncInFlightException)
            {
                // A background sync was mid-flight, so this toggle's sync was skipped
                // for backpressure. The requested Streaming state is already set, so
                // keep it and let a follow-up sync apply it instead of rolling back and
                // reporting a hard failure (this is the confusing "media-..." error).
                RunOnUiThread(() =>
                {
                    OutputStatus = starting ? "Streaming starting - applying..." : "Streaming stopping - applying...";
                    OutputSessionStatus = OutputStatus;
                });
                LaunchLog.Write($"stream: {(starting ? "start" : "stop")} deferred because media core sync is in flight");
                _ = RetryStreamSyncAsync(starting);
            }
            catch (Exception ex)
            {
                var action = starting ? "start" : "stop";
                var failureStatus = FormatStreamingFailureStatus(action, ex);
                LaunchLog.Write($"stream: {action} failed {ex.GetType().Name}: {ex.Message}");
                RunOnUiThread(() =>
                {
                    Streaming = previousStreaming;
                    RefreshOutputStatus();
                    OutputStatus = failureStatus;
                    OutputSessionStatus = OutputStatus;
                });

                if (starting && !previousStreaming)
                {
                    _ = SyncFailedStreamStartRollbackAsync(failureStatus);
                }
            }
        }
        finally
        {
            RunOnUiThread(() =>
            {
                _streamToggleInFlight = false;
                ToggleStreamingCommand.NotifyCanExecuteChanged();
            });
        }
    }

    private async Task SyncFailedStreamStartRollbackAsync(string failureStatus)
    {
        LaunchLog.Write($"stream: rollback after failed start {failureStatus}");
        for (var attempt = 0; attempt < 4; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Wait for the active sync to clear, then retry the explicit stop.
            }
            catch (Exception ex)
            {
                var rollbackFailure = NormalizeStreamingFailureDetail(ex.Message);
                RunOnUiThread(() =>
                {
                    OutputStatus = $"{failureStatus} Stream cleanup also failed: {rollbackFailure}";
                    OutputSessionStatus = OutputStatus;
                });
                return;
            }
        }

        RunOnUiThread(() =>
        {
            OutputStatus = $"{failureStatus} Stream cleanup is still waiting for media core.";
            OutputSessionStatus = OutputStatus;
        });
    }

    // Re-applies the production sync after a stream toggle was skipped for
    // backpressure, so the requested Streaming state actually reaches the media
    // core once the in-flight sync clears. Retries a few times, then gives up
    // quietly (the periodic sync will still converge).
    private async Task RetryStreamSyncAsync(bool starting)
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                var snapshot = await SyncActiveSceneAsync().ConfigureAwait(false);
                if (starting && TryFormatStreamingStartHealthFailure(snapshot, out var healthFailureStatus))
                {
                    LaunchLog.Write($"stream: retry start failed health proof {healthFailureStatus}");
                    RunOnUiThread(() =>
                    {
                        Streaming = ResolveStreamingStateAfterFailedRetry(starting);
                        RefreshOutputStatus();
                        OutputStatus = healthFailureStatus;
                        OutputSessionStatus = OutputStatus;
                    });
                    _ = SyncFailedStreamStartRollbackAsync(healthFailureStatus);
                    return;
                }

                if (starting)
                {
                    var requestedDestinations = BuildSelectedStreamDestinations(validatedOnly: true);
                    var proof = await WaitForStreamingStartProofAsync(requestedDestinations, snapshot).ConfigureAwait(false);
                    if (!string.IsNullOrWhiteSpace(proof.FailureStatus))
                    {
                        LaunchLog.Write($"stream: retry start failed sender proof {proof.FailureStatus}");
                        RunOnUiThread(() =>
                        {
                            Streaming = ResolveStreamingStateAfterFailedRetry(starting);
                            RefreshOutputStatus();
                            OutputStatus = proof.FailureStatus;
                            OutputSessionStatus = OutputStatus;
                        });
                        _ = SyncFailedStreamStartRollbackAsync(proof.FailureStatus);
                        return;
                    }
                }

                RunOnUiThread(() =>
                {
                    OutputStatus = starting ? "Streaming start requested." : "Streaming stopped.";
                    OutputSessionStatus = OutputStatus;
                    RefreshOutputStatus();
                });
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Still busy; try again shortly.
            }
            catch (Exception ex)
            {
                var failureStatus = FormatStreamingFailureStatus(starting ? "start" : "stop", ex);
                RunOnUiThread(() =>
                {
                    Streaming = ResolveStreamingStateAfterFailedRetry(starting);
                    RefreshOutputStatus();
                    OutputStatus = failureStatus;
                    OutputSessionStatus = OutputStatus;
                });

                if (starting)
                {
                    _ = SyncFailedStreamStartRollbackAsync(failureStatus);
                }
                return;
            }
        }

        var exhaustedStatus = FormatStreamSyncRetryExhaustedStatus(starting);
        RunOnUiThread(() =>
        {
            Streaming = ResolveStreamingStateAfterFailedRetry(starting);
            RefreshOutputStatus();
            OutputStatus = exhaustedStatus;
            OutputSessionStatus = OutputStatus;
        });

        if (starting)
        {
            _ = SyncFailedStreamStartRollbackAsync(exhaustedStatus);
        }
    }

    public static bool ResolveStreamingStateAfterFailedRetry(bool requestedStarting) => !requestedStarting;

    public static string FormatStreamSyncRetryExhaustedStatus(bool requestedStarting) =>
        requestedStarting
            ? "Streaming start failed: Media core is busy applying changes. Wait a moment and try Stream again."
            : "Streaming stop failed: Media core is busy applying changes. Wait a moment and try Stream again.";

    private async Task<(NativeMediaCoreStateSnapshot Snapshot, string? FailureStatus)> WaitForStreamingStartProofAsync(
        IReadOnlyList<string> requestedDestinations,
        NativeMediaCoreStateSnapshot initialSnapshot)
    {
        var snapshot = initialSnapshot;
        for (var attempt = 0; attempt < 6; attempt++)
        {
            if (TryFormatStreamingStartHealthFailure(snapshot, out var healthFailureStatus))
            {
                return (snapshot, healthFailureStatus);
            }

            if (IsStreamingStartProven(snapshot, requestedDestinations))
            {
                return (snapshot, null);
            }

            await Task.Delay(200).ConfigureAwait(false);
            snapshot = await _bridge.PollSnapshotAsync().ConfigureAwait(false);
        }

        return TryFormatStreamingStartNoSenderFailure(snapshot, requestedDestinations, out var failureStatus)
            ? (snapshot, failureStatus)
            : (snapshot, null);
    }

    private static bool IsStreamingStartProven(
        NativeMediaCoreStateSnapshot snapshot,
        IReadOnlyList<string> requestedDestinations)
    {
        if (requestedDestinations.Count == 0)
        {
            return false;
        }

        return requestedDestinations.Any(destination =>
            snapshot.OutputSenderSession.Senders.Any(sender =>
                sender.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase) &&
                sender.Status is "starting" or "live") ||
            snapshot.OutputHealth.Any(item =>
                item.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase) &&
                item.Status == "live"));
    }

    public static bool TryFormatStreamingStartNoSenderFailure(
        NativeMediaCoreStateSnapshot snapshot,
        IReadOnlyList<string> requestedDestinations,
        out string failureStatus)
    {
        failureStatus = string.Empty;
        if (requestedDestinations.Count == 0)
        {
            return false;
        }

        if (IsStreamingStartProven(snapshot, requestedDestinations))
        {
            return false;
        }

        var unavailableSender = snapshot.OutputSenderSession.Senders.FirstOrDefault(sender =>
            requestedDestinations.Any(destination => sender.Destination.Equals(destination, StringComparison.OrdinalIgnoreCase)) &&
            IsUnavailableOutputSenderWarning(sender));
        if (unavailableSender is not null)
        {
            var unavailableDetail = BuildOutputSenderFailureDetail(unavailableSender);
            failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(unavailableDetail));
            return true;
        }

        var destinations = string.Join(", ", requestedDestinations.Select(destination => destination.ToUpperInvariant()));
        var senderState = $"{snapshot.OutputSenderSession.Status}:{snapshot.OutputSenderSession.ActiveSenderCount}";
        var detail = $"Selected stream destinations ({destinations}) did not arm a native output sender. Sender state {senderState}.";
        failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(detail));
        return true;
    }

    private static bool IsUnavailableOutputSenderWarning(NativeMediaCoreOutputSender sender)
    {
        var detail = string.Join(
            " ",
            new[] { sender.LastResultCode, sender.Warning, sender.RuntimeDetail, sender.LastError }
                .Where(static part => !string.IsNullOrWhiteSpace(part)))
            .ToLowerInvariant();
        return detail.Contains("output-unavailable", StringComparison.Ordinal) ||
               detail.Contains("not available in this build", StringComparison.Ordinal) ||
               detail.Contains("runtime-missing", StringComparison.Ordinal) ||
               detail.Contains("libndi runtime", StringComparison.Ordinal) ||
               detail.Contains("no ndi sender module", StringComparison.Ordinal) ||
               detail.Contains("no srt sender module", StringComparison.Ordinal);
    }

    private static bool IsUnavailableOutputHealthWarning(NativeMediaCoreOutputHealth item)
    {
        var detail = item.Message?.ToLowerInvariant() ?? string.Empty;
        return detail.Contains("output-unavailable", StringComparison.Ordinal) ||
               detail.Contains("not available in this build", StringComparison.Ordinal) ||
               detail.Contains("runtime-missing", StringComparison.Ordinal) ||
               detail.Contains("libndi runtime", StringComparison.Ordinal) ||
               detail.Contains("no ndi sender module", StringComparison.Ordinal) ||
               detail.Contains("no srt sender module", StringComparison.Ordinal);
    }

    public static bool ResolveRecordingStateAfterFailedRetry(bool requestedStarting) => !requestedStarting;

    public static string FormatRecordingSyncRetryExhaustedStatus(bool requestedStarting) =>
        requestedStarting
            ? "Recording start failed: Media core is busy applying changes. Wait a moment and try Record again."
            : "Recording stop failed: Media core is busy applying changes. Wait a moment and try Record again.";

    public static bool TryFormatRecordingStartHealthFailure(NativeMediaCoreStateSnapshot snapshot, out string failureStatus)
    {
        var detail = snapshot.OutputHealth
            .Where(item =>
                item.Status is "failed" or "warning" &&
                item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase))
            .Select(item => item.Message)
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        detail ??= snapshot.Recording is { Active: true, Status: "failed" or "warning" } recording
            ? recording.Error ?? recording.Warning
            : null;

        if (string.IsNullOrWhiteSpace(detail))
        {
            failureStatus = string.Empty;
            return false;
        }

        failureStatus = FormatRecordingFailureStatus("start", new InvalidOperationException(detail));
        return true;
    }

    public static bool TryFormatStreamingStartHealthFailure(NativeMediaCoreStateSnapshot snapshot, out string failureStatus)
    {
        var detail = snapshot.OutputSenderSession.Senders
            .Where(static sender => sender.Status is "failed" or "warning")
            .Select(BuildOutputSenderFailureDetail)
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        detail ??= snapshot.OutputHealth
            .Where(item =>
                item.Status is "failed" or "warning" &&
                !item.Destination.Equals("recording", StringComparison.OrdinalIgnoreCase))
            .Select(item => item.Message)
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        detail ??= snapshot.OutputSenderSession.Warnings
            .FirstOrDefault(static item => !string.IsNullOrWhiteSpace(item));

        if (string.IsNullOrWhiteSpace(detail))
        {
            failureStatus = string.Empty;
            return false;
        }

        failureStatus = FormatStreamingFailureStatus("start", new InvalidOperationException(detail));
        return true;
    }

    private static string BuildOutputSenderFailureDetail(NativeMediaCoreOutputSender sender) =>
        string.Join(
            " ",
            new[] { sender.LastResultCode, sender.Warning, sender.RuntimeDetail, sender.LastError }
                .Where(static part => !string.IsNullOrWhiteSpace(part)));

    [RelayCommand]
    private void SetViewMode(string mode)
    {
        ViewMode = mode switch
        {
            "preview" => StudioViewMode.Preview,
            "program-preview" => StudioViewMode.ProgramPreview,
            "multiview" => StudioViewMode.Multiview,
            _ => StudioViewMode.Program
        };
    }

    [RelayCommand]
    private void SelectTab(string tab)
    {
        ActiveTab = tab switch
        {
            "settings" => StudioTab.Settings,
            "sources" or "scenes" => StudioTab.Sources,
            "inputs" => StudioTab.Inputs,
            "routing" => StudioTab.Routing,
            "overlays" => StudioTab.Overlays,
            "audio" => StudioTab.Audio,
            "media" => StudioTab.Media,
            "automation" => StudioTab.Automation,
            _ => StudioTab.Studio
        };
    }

    private List<RoutingSource> BuildAssignedInputSources()
    {
        var sources = new List<RoutingSource>();
        foreach (var input in ShowInputs.Where(slot => slot.IsAssigned && slot.Kind != ShowInputKind.Unassigned))
        {
            sources.Add(new RoutingSource(
                FormatInputSourceId(input.SlotNumber),
                $"{input.SlotLabel} - {ResolveShowInputSourceLabel(input)}"));
        }

        return sources;
    }

    private void BuildAudioRoutingMatrix()
    {
        var sources = BuildAssignedAudioSources();
        if (IsLocalAudioSourceConfigured(LocalAudioSourceEnabled, SelectedLocalAudioCaptureDeviceId, AudioCaptureDevices))
        {
            AddUniqueRoutingSource(sources, new RoutingSource(
                "local-machine-audio",
                ResolveLocalAudioRoutingSourceLabel(SelectedLocalAudioCaptureDeviceName)));
        }
        AddUniqueRoutingSource(sources, new RoutingSource("zoom-mix", "Zoom meeting mix (program default)"));
        AddUniqueRoutingSource(sources, new RoutingSource("media", "Media playback"));
        AudioRoutingMatrix.Build(sources);
        RefreshAudioProcessingTargets();
    }

    private List<RoutingSource> BuildAssignedAudioSources()
    {
        var sources = new List<RoutingSource>();
        foreach (var input in ShowInputs.Where(IsShowInputAudioSource))
        {
            var sourceId = ResolveAudioRoutingMatrixSourceId(FormatInputSourceId(input.SlotNumber));
            // Z1: ISO streams default unrouted (the meeting mix is program default).
            AddUniqueRoutingSource(sources, new RoutingSource(sourceId, $"{input.SlotLabel} - {ResolveShowInputSourceLabel(input)} (ISO)", DefaultUnrouted: true));
        }

        foreach (var captureDevice in CaptureDevices
            .Where(device => !string.Equals(device.Vendor, "srt", StringComparison.OrdinalIgnoreCase))
            .Where(device => !string.IsNullOrWhiteSpace(device.AssignedAudioDeviceId) ||
                !string.IsNullOrWhiteSpace(device.AssignedAudioDeviceName)))
        {
            AddUniqueRoutingSource(sources, new RoutingSource($"capture:{captureDevice.Id}", $"{captureDevice.Name} audio"));
        }

        return sources;
    }

    public static bool IsShowInputAudioSource(ShowInputSlot slot) =>
        slot.IsAssigned && slot.Kind == ShowInputKind.ZoomParticipant;

    private static void AddUniqueRoutingSource(ICollection<RoutingSource> sources, RoutingSource source)
    {
        if (string.IsNullOrWhiteSpace(source.Id) ||
            sources.Any(existing => string.Equals(existing.Id, source.Id, StringComparison.Ordinal)))
        {
            return;
        }

        sources.Add(source);
    }

    private void BuildVideoRoutingMatrix()
    {
        var sources = BuildAssignedInputSources();
        sources.Add(new RoutingSource("active-speaker", "Active Speaker"));
        sources.Add(new RoutingSource("screen-share", "Screen Share"));
        sources.Add(new RoutingSource("media", "Media"));
        VideoRoutingMatrix.Build(sources);
        foreach (var slot in ShowInputs.Where(slot => slot.IsAssigned && slot.Kind != ShowInputKind.Unassigned))
        {
            VideoRoutingMatrix.SetRoute(FormatInputSourceId(slot.SlotNumber), "multiview", slot.InShow);
        }
    }

    private void OnVideoRoutingMatrixChanged(VideoRoutingCrosspointViewModel cell)
    {
        if (cell.Destination.Id.StartsWith("iso-", StringComparison.OrdinalIgnoreCase))
        {
            _ = TrySyncMediaCoreAsync();
            CommandStatus = $"{cell.SourceLabel} {(cell.IsRouted ? "routed to" : "removed from")} {cell.Destination.Label}";
            return;
        }

        if (!string.Equals(cell.Destination.Id, "multiview", StringComparison.Ordinal) ||
            !TryParseInputSourceId(cell.SourceId, out var slotNumber) ||
            ShowInputs.FirstOrDefault(slot => slot.SlotNumber == slotNumber) is not { } slot)
        {
            return;
        }

        slot.InShow = cell.IsRouted;
        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
        SchedulePreviewRoutingRefresh();
        CommandStatus = $"{slot.SlotLabel} {(slot.InShow ? "routed to" : "removed from")} multiview";
    }

    private void OnAudioRoutingMatrixChanged(AudioRoutingCrosspointViewModel cell)
    {
        // Local-edit quiet period: while the operator's change is round-tripping
        // to the core, an in-flight snapshot still describes the PREVIOUS state;
        // hydrating from it would visibly revert the click. Local edits win for
        // a couple of seconds, then the core is authoritative again.
        _lastAudioRouteLocalEditAt = DateTimeOffset.UtcNow;
        RefreshAudioProcessingTargets();
        _ = TrySyncMediaCoreAsync();
        CommandStatus = $"{cell.SourceLabel} {(cell.IsRouted ? "routed to" : "removed from")} {cell.Bus.Label}";
    }

    private DateTimeOffset _lastAudioRouteLocalEditAt = DateTimeOffset.MinValue;

    // Phase B1 (audio tab redesign): the routing grid reflects the CORE's actual
    // send list from the snapshot instead of remaining a client-side ghost. The
    // core publishes engine source ids; rows are keyed by UI ids, so build the
    // reverse map through the same resolver the push path uses. Sends for
    // sources not currently in the grid (e.g. unassigned participants routed by
    // engine defaults) are skipped until the fuller roster lands in phase B4.
    private void HydrateAudioRoutingMatrixFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var native = snapshot.AudioRoutingMatrix;
        if (native is null || native.Sends.Count == 0 || AudioRoutingMatrix.Rows.Count == 0)
        {
            return;
        }

        if (DateTimeOffset.UtcNow - _lastAudioRouteLocalEditAt < TimeSpan.FromSeconds(2))
        {
            return;
        }

        var uiIdByEngineId = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var row in AudioRoutingMatrix.Rows)
        {
            var engineId = ResolveAudioRoutingMatrixSourceId(row.SourceId);
            if (!string.IsNullOrWhiteSpace(engineId) && !uiIdByEngineId.ContainsKey(engineId))
            {
                uiIdByEngineId[engineId] = row.SourceId;
            }
        }

        var sends = new List<(string SourceId, string BusId, double GainDb)>(native.Sends.Count);
        foreach (var send in native.Sends)
        {
            if (uiIdByEngineId.TryGetValue(send.SourceId, out var uiSourceId))
            {
                sends.Add((uiSourceId, send.BusId, send.GainDb));
            }
        }

        if (sends.Count > 0 && AudioRoutingMatrix.ApplyCoreSends(sends))
        {
            RefreshAudioProcessingTargets();
        }
    }

    private static string FormatInputSourceId(int slotNumber) => $"input-{slotNumber:00}";

    private static bool TryParseInputSourceId(string sourceId, out int slotNumber)
    {
        slotNumber = 0;
        return sourceId is { Length: 8 } &&
            sourceId.StartsWith("input-", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(sourceId[6..], out slotNumber);
    }

    private string ResolveShowInputSourceLabel(ShowInputSlot slot)
    {
        if (slot.Kind == ShowInputKind.ZoomParticipant &&
            slot.ParticipantId is { Length: > 0 } participantId)
        {
            return RoomVideoParticipants.FirstOrDefault(participant => participant.Id == participantId)?.Name ??
                participantId;
        }

        if (slot.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            return CaptureDevices.FirstOrDefault(device => device.Id == captureDeviceId)?.Name ??
                captureDeviceId;
        }

        return slot.KindLabel;
    }

    [RelayCommand]
    private void OpenSceneBuilder()
    {
        ActiveTab = StudioTab.Sources;
        CommandStatus = $"Editing {PreviewScene.Name} — drag sources on the Scenes canvas";
        OnPropertyChanged(nameof(SceneBuilderSlotSummary));
        OnPropertyChanged(nameof(PreviewSlotCount));
        OnPropertyChanged(nameof(HasPreviewSlotEditors));
    }

    [RelayCommand]
    private void SelectParticipant(string participantId)
    {
        SelectedParticipantId = participantId;
        SelectAudioProcessingTarget(FormatChannelProcessingTargetId(participantId));
    }

    public void SelectAudioProcessingTarget(string targetId)
    {
        if (string.IsNullOrWhiteSpace(targetId))
        {
            return;
        }

        if (AudioProcessingTargetOptions.All(target =>
                !string.Equals(target.Id, targetId, StringComparison.Ordinal)))
        {
            RefreshAudioProcessingTargets();
        }

        if (AudioProcessingTargetOptions.Any(target =>
                string.Equals(target.Id, targetId, StringComparison.Ordinal)))
        {
            SelectedAudioProcessingTargetId = targetId;
        }
    }

    [RelayCommand]
    private void OpenAudioMixer()
    {
        try
        {
            if (_audioMixerWindow is not null)
            {
                _audioMixerWindow.Activate();
                return;
            }

            _audioMixerWindow = new AudioMixerWindow(this);
            _audioMixerWindow.WindowClosed += OnAudioMixerWindowClosed;
            _audioMixerWindow.Activate();
            CommandStatus = "Audio mixer opened.";
        }
        catch (Exception ex)
        {
            if (_audioMixerWindow is not null)
            {
                _audioMixerWindow.WindowClosed -= OnAudioMixerWindowClosed;
                _audioMixerWindow = null;
            }

            CommandStatus = FormatAudioMixerFailureStatus(ex);
        }
    }

    private void OnAudioMixerWindowClosed(object? sender, EventArgs e)
    {
        if (_audioMixerWindow is not null)
        {
            _audioMixerWindow.WindowClosed -= OnAudioMixerWindowClosed;
            _audioMixerWindow = null;
        }
    }

    // Pop the multiviewer out to its own window (drag it to a second display). The MAIN view
    // unbinds the shared texture (StudioMultiviewSurface → null) so only the pop-out presents it.
    [RelayCommand]
    private void PopOutMultiview()
    {
        try
        {
            if (_multiviewPopoutWindow is not null)
            {
                _multiviewPopoutWindow.Activate();
                return;
            }

            MultiviewPoppedOut = true;  // release the texture from the main view FIRST
            _multiviewPopoutWindow = new MultiviewPopoutWindow(this);
            _multiviewPopoutWindow.WindowClosed += OnMultiviewPopoutWindowClosed;
            _multiviewPopoutWindow.Activate();
            CommandStatus = "Multiviewer popped out.";
        }
        catch (Exception ex)
        {
            MultiviewPoppedOut = false;
            if (_multiviewPopoutWindow is not null)
            {
                _multiviewPopoutWindow.WindowClosed -= OnMultiviewPopoutWindowClosed;
                _multiviewPopoutWindow = null;
            }

            CommandStatus = $"Could not pop out the multiviewer: {ex.Message}";
        }
    }

    // Bring the multiviewer back into the main window (also invoked when the pop-out is closed).
    [RelayCommand]
    private void PopInMultiview()
    {
        if (_multiviewPopoutWindow is not null)
        {
            var window = _multiviewPopoutWindow;
            _multiviewPopoutWindow = null;
            window.WindowClosed -= OnMultiviewPopoutWindowClosed;
            try
            {
                window.Close();
            }
            catch
            {
                // best effort
            }
        }

        MultiviewPoppedOut = false;  // re-bind the texture to the main view LAST
    }

    private void OnMultiviewPopoutWindowClosed(object? sender, EventArgs e)
    {
        if (_multiviewPopoutWindow is not null)
        {
            _multiviewPopoutWindow.WindowClosed -= OnMultiviewPopoutWindowClosed;
            _multiviewPopoutWindow = null;
        }

        MultiviewPoppedOut = false;
    }

    // Pop out a dedicated large PROGRAM + PREVIEW view to its own window. It presents the core's
    // dedicated program/preview shared textures (ProgramSurface/PreviewSurface) — SEPARATE textures
    // from the multiview wall, so both can present at once (no unbind needed, unlike the multiview).
    [RelayCommand]
    private void PopOutProgramPreview()
    {
        try
        {
            if (_programPreviewPopoutWindow is not null)
            {
                _programPreviewPopoutWindow.Activate();
                return;
            }

            _programPreviewPopoutWindow = new ProgramPreviewPopoutWindow(this);
            _programPreviewPopoutWindow.WindowClosed += OnProgramPreviewPopoutWindowClosed;
            _programPreviewPopoutWindow.Activate();
            CommandStatus = "Program/Preview popped out.";
        }
        catch (Exception ex)
        {
            if (_programPreviewPopoutWindow is not null)
            {
                _programPreviewPopoutWindow.WindowClosed -= OnProgramPreviewPopoutWindowClosed;
                _programPreviewPopoutWindow = null;
            }

            CommandStatus = $"Could not pop out Program/Preview: {ex.Message}";
        }
    }

    private void OnProgramPreviewPopoutWindowClosed(object? sender, EventArgs e)
    {
        if (_programPreviewPopoutWindow is not null)
        {
            _programPreviewPopoutWindow.WindowClosed -= OnProgramPreviewPopoutWindowClosed;
            _programPreviewPopoutWindow = null;
        }
    }

    public static string FormatAudioMixerFailureStatus(Exception exception)
    {
        var detail = string.IsNullOrWhiteSpace(exception.Message)
            ? exception.GetType().Name
            : exception.Message.Trim();
        return $"Audio mixer failed to open: {detail}";
    }

    public static string FormatAudioMixerActionFailureStatus(Exception exception)
    {
        var detail = string.IsNullOrWhiteSpace(exception.Message)
            ? exception.GetType().Name
            : exception.Message.Trim();
        return $"Audio mixer action failed: {detail}";
    }

    public void ReportAudioMixerFailure(Exception exception)
    {
        CommandStatus = FormatAudioMixerActionFailureStatus(exception);
    }

    [RelayCommand]
    private void OpenProductionSettings()
    {
        OpenProductionSettingsSection("output");
    }

    [RelayCommand]
    private void OpenProductionSettingsSection(string? section)
    {
        if (_productionSettingsWindow is not null)
        {
            _productionSettingsWindow.ShowSection(section);
            _productionSettingsWindow.Activate();
            return;
        }

        _productionSettingsWindow = new ProductionSettingsWindow(this);
        _productionSettingsWindow.WindowClosed += OnProductionSettingsWindowClosed;
        _productionSettingsWindow.ShowSection(section);
        _productionSettingsWindow.Activate();
    }

    private void OnProductionSettingsWindowClosed(object? sender, EventArgs e)
    {
        if (_productionSettingsWindow is not null)
        {
            _productionSettingsWindow.WindowClosed -= OnProductionSettingsWindowClosed;
            _productionSettingsWindow = null;
        }
    }

    [RelayCommand]
    private void OpenDiagnostics()
    {
        Settings.RefreshDiagnosticsReadout();

        if (_diagnosticsWindow is not null)
        {
            _diagnosticsWindow.Activate();
            return;
        }

        _diagnosticsWindow = new DiagnosticsWindow(this);
        _diagnosticsWindow.WindowClosed += OnDiagnosticsWindowClosed;
        _diagnosticsWindow.Activate();
    }

    private void OnDiagnosticsWindowClosed(object? sender, EventArgs e)
    {
        if (_diagnosticsWindow is not null)
        {
            _diagnosticsWindow.WindowClosed -= OnDiagnosticsWindowClosed;
            _diagnosticsWindow = null;
        }
    }

    public void SetMixerManualGain(string participantId, double gainDb)
    {
        var mix = _audioMixChannels.FirstOrDefault(item =>
            string.Equals(item.ParticipantId, participantId, StringComparison.Ordinal));
        if (mix is null)
        {
            return;
        }

        var normalizedGain = NormalizeMixerGain(gainDb);
        if (Math.Abs(mix.ManualGainDb - normalizedGain) < 0.05)
        {
            return;
        }

        mix.ManualGainDb = normalizedGain;
        RefreshMixerValueBindings(participantId);
        _ = TrySyncMediaCoreAsync();
    }

    public void SetMixerPan(string participantId, double pan)
    {
        var mix = _audioMixChannels.FirstOrDefault(item =>
            string.Equals(item.ParticipantId, participantId, StringComparison.Ordinal));
        if (mix is null)
        {
            return;
        }

        var normalizedPan = NormalizeMixerPan(pan);
        if (Math.Abs(mix.Pan - normalizedPan) < 0.005)
        {
            return;
        }

        mix.Pan = normalizedPan;
        RefreshMixerValueBindings(participantId);
        _ = TrySyncMediaCoreAsync();
    }

    private static double NormalizeMixerGain(double value) =>
        double.IsFinite(value)
            ? Math.Clamp(Math.Round(value, 1), -24, 24)
            : 0;

    private static double NormalizeMixerPan(double value) =>
        double.IsFinite(value)
            ? Math.Clamp(Math.Round(value, 2), -1, 1)
            : 0;

    private static double NormalizeMeterDb(double value) =>
        double.IsFinite(value)
            ? Math.Clamp(value, -120, 12)
            : -120;

    public void ToggleMixerMute(string participantId)
    {
        var mix = _audioMixChannels.FirstOrDefault(item =>
            string.Equals(item.ParticipantId, participantId, StringComparison.Ordinal));
        if (mix is null)
        {
            return;
        }

        SelectedParticipantId = participantId;
        mix.Muted = !mix.Muted;
        RefreshMixerValueBindings(participantId);
        RefreshAudioProcessingTargets();
        OnPropertyChanged(nameof(SelectedMuteButtonLabel));
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        CommandStatus = mix.Muted
            ? $"{SelectedParticipant?.Name} muted in mix"
            : $"{SelectedParticipant?.Name} unmuted in mix";
        _ = TrySyncMediaCoreAsync();
    }

    // B3: Solo has always been supported by the mix wire ("solo") and the core
    // routed-bus DSP — no UI ever exposed it until now.
    // ---- Production roles (scenes redesign R1) ------------------------------
    // Session-only by design: rosters change every show, so roles are assigned
    // fresh each session. Scene routes persist ROLE targets; people don't.
    private readonly Dictionary<string, string> _participantProductionRoles = new(StringComparer.Ordinal);

    public IReadOnlyList<RouteSelectOption> ProductionRoleAssignmentOptions { get; } =
        ProductionRoleService.AssignmentOptions;

    public void SetParticipantProductionRole(string participantId, string? roleId)
    {
        var participant = RoomVideoParticipants.FirstOrDefault(item =>
            string.Equals(item.Id, participantId, StringComparison.Ordinal));
        if (participant is null)
        {
            return;
        }

        var normalized = string.IsNullOrWhiteSpace(roleId) ? null : roleId.Trim();
        _participantProductionRoles.TryGetValue(participantId, out var current);
        if (string.Equals(current, normalized, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        if (normalized is null)
        {
            _participantProductionRoles.Remove(participantId);
            CommandStatus = $"{participant.Name} role cleared";
        }
        else
        {
            // One holder per role: assigning Host to B strips it from A.
            var previousHolder = _participantProductionRoles
                .FirstOrDefault(pair => string.Equals(pair.Value, normalized, StringComparison.OrdinalIgnoreCase))
                .Key;
            if (previousHolder is not null)
            {
                _participantProductionRoles.Remove(previousHolder);
            }

            _participantProductionRoles[participantId] = normalized;
            CommandStatus = $"{participant.Name} is now {ProductionRoleService.RoleLabel(normalized)}";
        }

        // Role holders changed -> role-targeted routes resolve differently.
        RefreshProductionReadouts();
        SchedulePreviewRoutingRefresh();
        _ = TrySyncMediaCoreAsync();
    }

    public void ToggleMixerSolo(string participantId)
    {
        var mix = _audioMixChannels.FirstOrDefault(item =>
            string.Equals(item.ParticipantId, participantId, StringComparison.Ordinal));
        if (mix is null)
        {
            return;
        }

        SelectedParticipantId = participantId;
        mix.Solo = !mix.Solo;
        RefreshMixerValueBindings(participantId);
        RefreshAudioProcessingTargets();
        CommandStatus = mix.Solo
            ? $"{SelectedParticipant?.Name} soloed in mix"
            : $"{SelectedParticipant?.Name} solo cleared";
        _ = TrySyncMediaCoreAsync();
    }

    // ---- C2 rack: quick toggles for the built-in processors the channel
    // insert chain actually runs (spec 4.4 / PR #178). Enabled = the
    // recognized insert name is on the selected channel's chain.
    public bool SelectedRackGateEnabled => SelectedChannelHasInsert("Noise Gate");
    public bool SelectedRackEqEnabled => SelectedChannelHasInsert("Built-in EQ");
    public bool SelectedRackCompressorEnabled => SelectedChannelHasInsert("Compressor");
    public bool SelectedRackLimiterEnabled => SelectedChannelHasInsert("Limiter");

    private bool SelectedChannelHasInsert(string insertName) =>
        SelectedAudioMix?.PluginInserts.Any(insert =>
            string.Equals(insert, insertName, StringComparison.OrdinalIgnoreCase)) == true;

    private void NotifySelectedRackChanged()
    {
        OnPropertyChanged(nameof(SelectedRackGateEnabled));
        OnPropertyChanged(nameof(SelectedRackEqEnabled));
        OnPropertyChanged(nameof(SelectedRackCompressorEnabled));
        OnPropertyChanged(nameof(SelectedRackLimiterEnabled));
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        OnPropertyChanged(nameof(SelectedChannelDisplayName));
        OnPropertyChanged(nameof(HasSelectedAudioChannel));
        OnPropertyChanged(nameof(WorkspaceEnabledOpacity));
        NotifyWorkspaceParamsChanged();  // C6 sliders + curves follow the channel
        RefreshSelectedChannelInsertSlots();  // C5a rack slots follow the chain
    }

    // ---- VST host P1: operator-initiated plugin discovery (spec 4.5). The
    // scan request rides the next sync as a one-shot command; results arrive
    // on the snapshot (audioMixSession.pluginHost).
    private bool _vstScanRequested;

    private bool ConsumeVstScanRequest()
    {
        var requested = _vstScanRequested;
        _vstScanRequested = false;
        return requested;
    }

    public void RequestVstPluginScan()
    {
        _vstScanRequested = true;
        CommandStatus = "Scanning VST3 plugins…";
        if (_applyingProductionPatch)
        {
            QueueProductionSyncRetry("vst-scan-during-snapshot");
            return;
        }
        _ = TrySyncMediaCoreAsync();
    }

    // U1a: the first time a plugin surface is opened this session, scan
    // automatically — a first-time user should never meet an empty browser and
    // a mystery Scan button. Manual Rescan stays for picking up new installs.
    private bool _vstAutoScanKicked;

    public void EnsureVstPluginScan()
    {
        if (_vstAutoScanKicked)
        {
            return;
        }

        var status = _bridge.LastSnapshot?.AudioMixSession.PluginHost.Status ?? "absent";
        if (status is "absent" or "error")
        {
            _vstAutoScanKicked = true;
            RequestVstPluginScan();
        }
    }

    // CACHED between snapshots: the browser's ItemsRepeater must NOT get a new
    // ItemsSource instance per snapshot apply (bound-collection churn is the
    // CoreMessagingXP 0xc000027b fail-fast class — see CLAUDE.md). The list
    // reference only changes when the scan results (or the search filter)
    // actually change.
    private IReadOnlyList<NativeMediaCorePluginInfo> _vstPlugins = [];
    private string _vstPluginHostSignature = "";
    private IReadOnlyList<NativeMediaCorePluginInfo> _filteredVstPlugins = [];
    private string _vstFilteredSignature = "";
    private string _vstServeSignature = "";
    private string _vstPluginFilter = "";
    private DateTime? _vstScanObservedAt;

    public IReadOnlyList<NativeMediaCorePluginInfo> VstPlugins => _vstPlugins;

    /// <summary>U1a: browser search filter (name or vendor, case-insensitive).</summary>
    public string VstPluginFilter
    {
        get => _vstPluginFilter;
        set
        {
            var normalized = value?.Trim() ?? "";
            if (string.Equals(_vstPluginFilter, normalized, StringComparison.Ordinal))
            {
                return;
            }

            _vstPluginFilter = normalized;
            RefreshFilteredVstPlugins();
        }
    }

    public IReadOnlyList<NativeMediaCorePluginInfo> FilteredVstPlugins => _filteredVstPlugins;

    public string VstHostStatus =>
        _bridge.LastSnapshot?.AudioMixSession.PluginHost.Status ?? "absent";

    public string VstPluginHostSummary =>
        (_bridge.LastSnapshot?.AudioMixSession.PluginHost.Status ?? "absent") switch
        {
            "ready" => $"{_vstPlugins.Sum(plugin => plugin.ClassNames.Count)} VST3 plug-in(s) available",
            "probing" => "Checking installed VST3 plug-ins…",
            "scanning" => "Finding installed VST3 plug-ins…",
            "error" => "VST3 scan needs attention — open Health for details",
            _ => "Checking for installed VST3 plug-ins…"
        };

    private void RefreshVstPluginHostFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var host = snapshot.AudioMixSession.PluginHost;
        var signature = host.Status + "|" + string.Join(
            "\u001f",
            host.Plugins.Select(plugin =>
                $"{plugin.Id}\u001e{plugin.Probe}\u001e{string.Join("\u001d", plugin.ClassNames)}"));
        if (!string.Equals(signature, _vstPluginHostSignature, StringComparison.Ordinal))
        {
            _vstPluginHostSignature = signature;
            _vstPlugins = host.Plugins;
            if (string.Equals(host.Status, "ready", StringComparison.Ordinal))
            {
                _vstScanObservedAt = DateTime.Now;
            }
            OnPropertyChanged(nameof(VstPlugins));
            OnPropertyChanged(nameof(VstHostStatus));
            OnPropertyChanged(nameof(VstPluginHostSummary));
            RefreshFilteredVstPlugins();
        }

        var serve = host.Serve;
        var serveSignature = $"{serve.Running}|{serve.StatusCode}|{serve.ActivePlugin}|{serve.LastError}|" +
                             $"{serve.DeadlineMisses}|{(serve.Exchanges > 0)}|" +
                             $"{serve.EditorStatusCode}|{serve.EditorActivePlugin}|{serve.EditorLastError}";
        if (!string.Equals(serveSignature, _vstServeSignature, StringComparison.Ordinal))
        {
            _vstServeSignature = serveSignature;
            OnPropertyChanged(nameof(VstBridgeStatusLabel));
            OnPropertyChanged(nameof(ProcessingBridgeStatusLabel));
            RefreshSelectedChannelInsertSlots();
            RefreshSelectedAudioProcessingSlots();
        }
    }

    private void RefreshFilteredVstPlugins()
    {
        var signature = _vstPluginHostSignature + "|filter:" + _vstPluginFilter.ToLowerInvariant();
        if (string.Equals(signature, _vstFilteredSignature, StringComparison.Ordinal))
        {
            return;
        }

        _vstFilteredSignature = signature;
        _filteredVstPlugins = _vstPluginFilter.Length == 0
            ? _vstPlugins
            : _vstPlugins
                .Where(plugin =>
                    plugin.Name.Contains(_vstPluginFilter, StringComparison.OrdinalIgnoreCase) ||
                    plugin.Vendor.Contains(_vstPluginFilter, StringComparison.OrdinalIgnoreCase) ||
                    plugin.ClassNames.Any(className =>
                        className.Contains(_vstPluginFilter, StringComparison.OrdinalIgnoreCase)))
                .ToList();
        OnPropertyChanged(nameof(FilteredVstPlugins));
    }

    // ---- C3/U2c: the Audio tab's three surfaces. SHOW = the console (mix a
    // live show); ROUTING = the full-width bus cards + crosspoint matrix
    // (owner 2026-07-12: routing deserves its own surface, not a squeezed
    // SETUP column); SETUP = devices + processing configuration.
    private string _audioTabMode = "show";

    public bool IsAudioShowMode => string.Equals(_audioTabMode, "show", StringComparison.Ordinal);

    public bool IsAudioRoutingSurface => string.Equals(_audioTabMode, "routing", StringComparison.Ordinal);

    public bool IsAudioSetupMode => string.Equals(_audioTabMode, "setup", StringComparison.Ordinal);

    public void SetAudioTabMode(string mode)
    {
        var normalized = mode?.ToLowerInvariant() switch
        {
            "setup" => "setup",
            "routing" => "routing",
            _ => "show",
        };
        if (string.Equals(_audioTabMode, normalized, StringComparison.Ordinal))
        {
            return;
        }

        _audioTabMode = normalized;
        OnPropertyChanged(nameof(IsAudioShowMode));
        OnPropertyChanged(nameof(IsAudioRoutingSurface));
        OnPropertyChanged(nameof(IsAudioSetupMode));
    }

    public void ToggleSelectedRackInsert(string insertName)
    {
        if (SelectedAudioMix is not { } mix || string.IsNullOrWhiteSpace(insertName))
        {
            return;
        }

        var existing = mix.PluginInserts.FirstOrDefault(insert =>
            string.Equals(insert, insertName, StringComparison.OrdinalIgnoreCase));
        if (existing is not null)
        {
            mix.PluginInserts.Remove(existing);
            CommandStatus = $"{insertName} removed from {SelectedParticipant?.Name ?? "selected channel"}";
        }
        else
        {
            mix.PluginInserts.Add(insertName);
            CommandStatus = $"{insertName} processing {SelectedParticipant?.Name ?? "selected channel"}";
        }

        RefreshMixerValueBindings(mix.ParticipantId);
        RefreshAudioProcessingTargets();
        NotifySelectedRackChanged();
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private void AddSelectedAudioInsert(string insertName)
    {
        if (SelectedAudioMix is not { } mix || string.IsNullOrWhiteSpace(insertName))
        {
            return;
        }

        var normalized = NormalizeAudioInsertName(insertName);
        if (mix.PluginInserts.Any(insert => string.Equals(insert, normalized, StringComparison.OrdinalIgnoreCase)))
        {
            CommandStatus = $"{normalized} is already on {SelectedParticipant?.Name ?? "selected channel"}";
            return;
        }

        mix.PluginInserts.Add(normalized);
        CommandStatus = normalized.StartsWith("VST", StringComparison.OrdinalIgnoreCase)
            ? $"Added {normalized}. The isolated VST3 host will start when audio reaches this insert."
            : $"Added {normalized} to {SelectedParticipant?.Name ?? "selected channel"}";
        RefreshMixerValueBindings(mix.ParticipantId);
        RefreshAudioProcessingTargets();
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private void ClearSelectedAudioInserts()
    {
        if (SelectedAudioMix is not { } mix)
        {
            return;
        }

        mix.PluginInserts.Clear();
        CommandStatus = $"Insert chain cleared for {SelectedParticipant?.Name ?? "selected channel"}";
        RefreshMixerValueBindings(mix.ParticipantId);
        RefreshAudioProcessingTargets();
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        _ = TrySyncMediaCoreAsync();
    }

    private static string NormalizeAudioInsertName(string insertName) =>
        insertName.Trim().ToLowerInvariant() switch
        {
            "eq" or "built-in eq" => "Built-in EQ",
            "compressor" => "Compressor",
            "vst" or "vst3" or "vst3 bridge" => "VST3 Bridge Slot",
            _ => insertName.Trim()
        };

    [RelayCommand]
    private void AddAudioProcessingInsert(string insertName)
    {
        if (string.IsNullOrWhiteSpace(insertName))
        {
            return;
        }

        var targetId = NormalizeAudioProcessingTargetId(SelectedAudioProcessingTargetId);
        if (string.IsNullOrWhiteSpace(targetId))
        {
            CommandStatus = "Choose an audio processing target first";
            return;
        }

        var normalized = NormalizeAudioInsertName(insertName);
        var inserts = ResolveAudioProcessingInsertList(targetId);
        if (inserts.Any(insert => string.Equals(insert, normalized, StringComparison.OrdinalIgnoreCase)))
        {
            CommandStatus = $"{normalized} is already on {ResolveAudioProcessingTargetName(targetId)}";
            return;
        }

        inserts.Add(normalized);
        CommandStatus = $"{normalized} added to {ResolveAudioProcessingTargetName(targetId)}";
        RefreshAudioProcessingTargets();
        RefreshSelectedAudioProcessingSlots();
        RefreshAudioParticipantRows();
        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
    }

    public void AddVstInsertToSelectedProcessingTarget(string pluginClassName)
    {
        if (string.IsNullOrWhiteSpace(pluginClassName))
        {
            return;
        }

        var insertName = pluginClassName.StartsWith("VST:", StringComparison.OrdinalIgnoreCase)
            ? pluginClassName.Trim()
            : $"VST:{pluginClassName.Trim()}";
        AddAudioProcessingInsert(insertName);
    }

    [RelayCommand]
    private void ClearAudioProcessingInserts()
    {
        var targetId = NormalizeAudioProcessingTargetId(SelectedAudioProcessingTargetId);
        if (string.IsNullOrWhiteSpace(targetId))
        {
            return;
        }

        ResolveAudioProcessingInsertList(targetId).Clear();
        CommandStatus = $"Insert chain cleared for {ResolveAudioProcessingTargetName(targetId)}";
        RefreshAudioProcessingTargets();
        RefreshSelectedAudioProcessingSlots();
        RefreshAudioParticipantRows();
        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
    }

    private List<string> ResolveAudioProcessingInsertList(string targetId)
    {
        if (TryParseChannelProcessingTargetId(targetId, out var participantId) &&
            _audioMixChannels.FirstOrDefault(mix =>
                string.Equals(mix.ParticipantId, participantId, StringComparison.Ordinal)) is { } channelMix)
        {
            return channelMix.PluginInserts;
        }

        if (!_audioProcessingInserts.TryGetValue(targetId, out var inserts))
        {
            inserts = [];
            _audioProcessingInserts[targetId] = inserts;
        }

        return inserts;
    }

    private IReadOnlyList<string> ResolveAudioProcessingInserts(string targetId) =>
        ResolveAudioProcessingInsertList(targetId);

    private string ResolveAudioProcessingTargetName(string targetId)
    {
        if (TryParseChannelProcessingTargetId(targetId, out var participantId))
        {
            return RoomVideoParticipants.FirstOrDefault(participant =>
                string.Equals(participant.Id, participantId, StringComparison.Ordinal))?.Name ??
                "selected channel";
        }

        return AudioRoutingMatrix.BusHeaders.FirstOrDefault(bus =>
            string.Equals(FormatBusProcessingTargetId(bus.Id), targetId, StringComparison.Ordinal))?.Label ??
            targetId;
    }

    private static string NormalizeAudioProcessingTargetId(string? targetId) =>
        string.IsNullOrWhiteSpace(targetId) ? "bus:master" : targetId.Trim();

    private static string FormatChannelProcessingTargetId(string participantId) => $"channel:{participantId}";

    private static string FormatBusProcessingTargetId(string busId) => $"bus:{busId}";

    private static bool TryParseChannelProcessingTargetId(string targetId, out string participantId)
    {
        participantId = string.Empty;
        if (!targetId.StartsWith("channel:", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        participantId = targetId["channel:".Length..];
        return participantId.Length > 0;
    }

    private void RefreshMixerBindings(string participantId, bool selectParticipant = true)
    {
        if (selectParticipant && !string.Equals(SelectedParticipantId, participantId, StringComparison.Ordinal))
        {
            SelectedParticipantId = participantId;
        }

        if (selectParticipant)
        {
            SelectedAudioProcessingTargetId = FormatChannelProcessingTargetId(participantId);
        }

        RefreshAudioParticipantRows();
        RefreshAudioProcessingTargets();
        RefreshAudioReadoutBindings();
        OnPropertyChanged(nameof(SelectedAudioMix));
        OnPropertyChanged(nameof(SelectedGainLabel));
        OnPropertyChanged(nameof(SelectedManualGainLabel));
        OnPropertyChanged(nameof(SelectedMuteButtonLabel));
        OnPropertyChanged(nameof(SelectedOutputLevel));
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        OnPropertyChanged(nameof(VstBridgeStatusLabel));
    }

    private void RefreshMixerValueBindings(string participantId)
    {
        RefreshAudioParticipantRow(participantId);
        RefreshAudioReadoutBindings();
        OnPropertyChanged(nameof(SelectedAudioMix));
        OnPropertyChanged(nameof(SelectedGainLabel));
        OnPropertyChanged(nameof(SelectedManualGainLabel));
        OnPropertyChanged(nameof(SelectedOutputLevel));
        OnPropertyChanged(nameof(VstBridgeStatusLabel));
    }

    /// <summary>
    /// Opens the per-source color grade pop-out for a Zoom participant or capture source.
    /// Saved grades are attached to matching scene routes and sent to native per layer.
    /// </summary>
    [RelayCommand]
    private void OpenColorGradeEditor(string? sourceId)
    {
        var normalizedSourceId = NormalizeColorGradeSourceId(sourceId);
        if (string.IsNullOrWhiteSpace(normalizedSourceId))
        {
            CommandStatus = "Select a source before editing its color grade";
            return;
        }

        var sourceName = ResolveColorGradeSourceName(normalizedSourceId);

        var seed = ResolveStoredColorGrade(normalizedSourceId);
        var editorViewModel = new ColorGradeEditorViewModel(
            normalizedSourceId,
            sourceName,
            seed,
            ResolveColorGradePreviewSurface(normalizedSourceId));
        editorViewModel.GradeChanged += OnSourceColorGradeChanged;
        editorViewModel.GradeSaved += OnSourceColorGradeSaved;

        var window = new ColorGradeEditorWindow(editorViewModel);
        window.Closed += (_, _) =>
        {
            editorViewModel.GradeChanged -= OnSourceColorGradeChanged;
            editorViewModel.GradeSaved -= OnSourceColorGradeSaved;
            _openColorGradeEditors.Remove(editorViewModel);
        };
        _openColorGradeEditors.Add(editorViewModel);
        window.Activate();
    }

    [RelayCommand]
    private void OpenCaptureDeviceColorGradeEditor(string? captureDeviceId) =>
        OpenColorGradeEditor(string.IsNullOrWhiteSpace(captureDeviceId) ? null : $"capture:{captureDeviceId}");

    private void OnSourceColorGradeSaved(object? sender, ColorGrade grade)
    {
        if (sender is not ColorGradeEditorViewModel editorViewModel)
        {
            return;
        }

        ApplyLiveColorGrade(editorViewModel, grade, $"Color grade set for {editorViewModel.SourceName}: {grade.Summary}");
    }

    private void OnSourceColorGradeChanged(object? sender, ColorGrade grade)
    {
        if (sender is not ColorGradeEditorViewModel editorViewModel)
        {
            return;
        }

        ApplyLiveColorGrade(editorViewModel, grade, $"Color grade live for {editorViewModel.SourceName}: {grade.Summary}");
    }

    private void ApplyLiveColorGrade(ColorGradeEditorViewModel editorViewModel, ColorGrade grade, string status)
    {
        _sourceColorGrades[editorViewModel.SourceId] = grade;
        ApplyColorGradeToMatchingRoutes(editorViewModel.SourceId, grade);
        CommandStatus = status;

        SyncPreviewCanvasLayers(GetPreviewEditableRoutes());
        RefreshPreviewRoutingState();
        _ = SyncColorGradeChangeAsync();
    }

    private VideoSurfaceState? ResolveColorGradePreviewSurface(string sourceId)
    {
        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            var captureDeviceId = sourceId["capture:".Length..];
            return _surfaces.CaptureDeviceSurfaces.TryGetValue(captureDeviceId, out var captureSurface)
                ? captureSurface
                : BuildCaptureSceneTile(captureDeviceId)?.Surface;
        }

        return MultiviewTiles
            .FirstOrDefault(tile => string.Equals(tile.Participant.Id, sourceId, StringComparison.Ordinal))
            ?.Surface;
    }

    private void RefreshOpenColorGradeEditorPreviews()
    {
        foreach (var editor in _openColorGradeEditors.ToList())
        {
            editor.SetPreviewSurface(ResolveColorGradePreviewSurface(editor.SourceId));
        }
    }

    private async Task SyncColorGradeChangeAsync()
    {
        try
        {
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            RunOnUiThread(() => CommandStatus = ex.Message);  // catch runs off-thread (ConfigureAwait(false))
        }
    }

    private string? NormalizeColorGradeSourceId(string? sourceId)
    {
        if (string.IsNullOrWhiteSpace(sourceId))
        {
            return null;
        }

        if (sourceId.StartsWith("input-", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(sourceId[6..], out var slotNumber) &&
            ShowInputs.FirstOrDefault(slot => slot.SlotNumber == slotNumber) is { } slot)
        {
            return slot.Kind == ShowInputKind.ZoomParticipant
                ? slot.ParticipantId
                : string.IsNullOrWhiteSpace(slot.CaptureDeviceId) ? null : $"capture:{slot.CaptureDeviceId}";
        }

        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            var captureDeviceId = sourceId["capture:".Length..];
            return string.IsNullOrWhiteSpace(captureDeviceId) ? null : $"capture:{captureDeviceId}";
        }

        if (CaptureDevices.Any(device => string.Equals(device.Id, sourceId, StringComparison.Ordinal)))
        {
            return $"capture:{sourceId}";
        }

        return sourceId;
    }

    private string ResolveColorGradeSourceName(string sourceId)
    {
        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            var captureDeviceId = sourceId["capture:".Length..];
            return CaptureDevices.FirstOrDefault(device => string.Equals(device.Id, captureDeviceId, StringComparison.Ordinal))?.Name ??
                captureDeviceId;
        }

        return RoomVideoParticipants.FirstOrDefault(participant => participant.Id == sourceId)?.Name ?? sourceId;
    }

    private ColorGrade ResolveStoredColorGrade(string sourceId) =>
        _sourceColorGrades.TryGetValue(sourceId, out var stored) ? stored : ColorGrade;

    private void ApplyColorGradeToMatchingRoutes(string sourceId, ColorGrade grade)
    {
        foreach (var route in _sceneRoutes.Values.SelectMany(routes => routes))
        {
            var resolved = ResolveRouteFromShowInput(route);
            if (string.Equals(ResolveColorGradeSourceId(resolved), sourceId, StringComparison.Ordinal))
            {
                route.ColorGrade = grade;
            }
        }
    }

    private MediaCoreColorGradeWire? BuildRouteColorGradeWire(SourceRoute route)
    {
        var sourceId = ResolveColorGradeSourceId(route);
        var grade = route.ColorGrade;
        if (sourceId is not null && _sourceColorGrades.TryGetValue(sourceId, out var stored))
        {
            grade = stored;
        }

        return grade is null
            ? null
            : new MediaCoreColorGradeWire(
                grade.Lut,
                grade.Exposure,
                grade.Contrast,
                grade.Saturation,
                grade.Temperature);
    }

    private static string? ResolveColorGradeSourceId(SourceRoute route)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice && route.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            return $"capture:{captureDeviceId}";
        }

        return route.ParticipantId;
    }

    [RelayCommand]
    private async Task ToggleGraphicAsync(string graphicId)
    {
        var graphic = Graphics.FirstOrDefault(g => g.Id == graphicId);
        if (graphic is null)
        {
            return;
        }

        graphic.Enabled = !graphic.Enabled;
        CommandStatus = $"{graphic.Name} {(graphic.Enabled ? "enabled" : "disabled")} on program";
        OnPropertyChanged(nameof(EnabledGraphics));
        RefreshProgramLowerThirdKeyPosition();

        if (_bridge.Running)
        {
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                RunOnUiThread(() => CommandStatus = ex.Message);  // catch runs off-thread (ConfigureAwait(false))
            }
        }
    }

    [RelayCommand]
    private void ToggleProgramLowerThird()
    {
        if (ProgramLowerThirdKey.IsVisible || ProgramLowerThirdEnabled)
        {
            ProgramLowerThirdEnabled = false;
            _programLowerThirdAutomationSuppressed = true;
            foreach (var graphic in Graphics.Where(graphic =>
                graphic.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase)))
            {
                graphic.Enabled = false;
            }

            CommandStatus = "Lower third keyed out";
        }
        else
        {
            if (ResolveProgramLowerThirdSource(ProgramSceneRoutes) is null)
            {
                CommandStatus = "Lower third needs a program source";
                OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
                OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
                OnPropertyChanged(nameof(StudioLowerThirdToolTip));
                return;
            }

            _programLowerThirdAutomationSuppressed = false;
            ProgramLowerThirdEnabled = true;
            CommandStatus = "Lower third keyed in";
        }

        OnPropertyChanged(nameof(EnabledGraphics));
        RefreshProgramLowerThirdKeyPosition();
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private void RebuildProgramLowerThird()
    {
        if (ResolveProgramLowerThirdSource(ProgramSceneRoutes) is not { } source)
        {
            CommandStatus = "Lower third needs a program source";
            OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
            OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
            OnPropertyChanged(nameof(StudioLowerThirdToolTip));
            return;
        }

        _programLowerThirdAutomationSuppressed = false;
        ProgramLowerThirdEnabled = true;
        UpdateProgramLowerThirdKey(source, force: true);
        CommandStatus = "Lower third rebuilt";
        _ = TrySyncMediaCoreAsync();
    }

    public IReadOnlyList<GraphicOverlay> EnabledGraphics =>
        Graphics.Where(graphic => graphic.Enabled).ToList();

    public bool AddGraphicOverlay(string kind)
    {
        var normalizedKind = string.IsNullOrWhiteSpace(kind) ? "lower-third" : kind.Trim().ToLowerInvariant();
        if (normalizedKind == "lower-third" &&
            ResolveProgramLowerThirdSource(ProgramSceneRoutes) is null)
        {
            CommandStatus = "Lower third needs a program source";
            OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
            OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
            OnPropertyChanged(nameof(StudioLowerThirdToolTip));
            return false;
        }

        var label = normalizedKind switch
        {
            "bug" => "Corner bug",
            "image" => "Image overlay",
            "caption" => "Caption strip",
            _ => "Lower third"
        };
        var position = normalizedKind switch
        {
            "bug" => "top-right",
            "image" => "center",
            "caption" => "bottom",
            _ => Overlays.LowerThirdPosition
        };

        Graphics.Add(new GraphicOverlay
        {
            Id = $"graphic-{DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()}",
            Name = label,
            Kind = normalizedKind,
            Position = position,
            Accent = BrandKit.AccentColor,
            Enabled = true
        });

        OnPropertyChanged(nameof(EnabledGraphics));
        RefreshProgramLowerThirdKeyPosition();
        CommandStatus = $"{label} added";
        _ = TrySyncMediaCoreAsync();
        return true;
    }

    [RelayCommand]
    private void ToggleAutomation()
    {
        if (!CanToggleSetAndForget() && ProductionMode != ProductionMode.SetAndForget)
        {
            CommandStatus = "Set & Forget is locked on this license tier";
            return;
        }

        ProductionMode = ProductionMode == ProductionMode.SetAndForget
            ? ProductionMode.Manual
            : ProductionMode.SetAndForget;
        AutomationButtonLabel = ProductionMode == ProductionMode.SetAndForget
            ? "Automation enabled"
            : "Automation disabled";
        RefreshProductionReadouts();
        CommandStatus = ProductionMode == ProductionMode.SetAndForget
            ? $"Set & Forget enabled: {_automationRecommendation.Reason}"
            : "Manual mode — operator controls scenes";
        RefreshTransportAutomationState();
    }

    [RelayCommand]
    private void RunMagicScene()
    {
        if (!Settings.IsInMeeting)
        {
            CommandStatus = "Magic Scene requires an active Zoom meeting";
            return;
        }

        var recommendedSceneId = _automationRecommendation.RecommendedSceneId;
        PreviewSceneId = recommendedSceneId;
        var sceneName = RecommendedSceneName;
        MagicSceneStatus = $"Magic Scene applied: {sceneName} queued on preview";
        ApplyAutomationOverlayPolicy();
        SchedulePreviewRoutingRefresh();
        CommandStatus = $"{sceneName} queued by Magic Scene";
        RefreshSceneItems();
    }

    private void EvaluateAutomationPolicy()
    {
        if (ProductionMode != ProductionMode.SetAndForget)
        {
            if (_automationTimer.IsRunning)
            {
                _automationTimer.Stop();
            }

            _automationPendingSceneId = null;
            _automationPendingSince = null;
            AutomationLastAction = "Manual mode - automation is not changing scenes";
            return;
        }

        if (!_automationTimer.IsRunning)
        {
            _automationTimer.Start();
        }

        ApplyAutomationOverlayPolicy();

        if (RoomVideoParticipants.Count == 0)
        {
            _automationPendingSceneId = null;
            _automationPendingSince = null;
            AutomationLastAction = "Waiting for meeting participants";
            return;
        }

        if (_automationRecommendation.Confidence < AutomationConfidenceThreshold)
        {
            _automationPendingSceneId = null;
            _automationPendingSince = null;
            AutomationLastAction = $"Holding current scene - confidence {_automationRecommendation.Confidence}% is below {AutomationConfidenceThreshold:0}%";
            return;
        }

        var targetSceneId = _automationRecommendation.RecommendedSceneId;
        if (string.Equals(targetSceneId, ActiveSceneId, StringComparison.Ordinal))
        {
            _automationPendingSceneId = null;
            _automationPendingSince = null;
            AutomationLastAction = $"{RecommendedSceneName} is already on program";
            return;
        }

        var now = DateTimeOffset.UtcNow;
        if (!string.Equals(_automationPendingSceneId, targetSceneId, StringComparison.Ordinal))
        {
            _automationPendingSceneId = targetSceneId;
            _automationPendingSince = now;
            AutomationLastAction = $"Holding {RecommendedSceneName} for {AutomationSwitchDelaySeconds:0}s before switching";
            return;
        }

        var elapsedSeconds = _automationPendingSince is { } pendingSince
            ? (now - pendingSince).TotalSeconds
            : 0;
        if (elapsedSeconds < AutomationSwitchDelaySeconds)
        {
            AutomationLastAction = $"Holding {RecommendedSceneName}: {elapsedSeconds:0.0}/{AutomationSwitchDelaySeconds:0}s";
            return;
        }

        if (!string.Equals(PreviewSceneId, targetSceneId, StringComparison.Ordinal))
        {
            PreviewSceneId = targetSceneId;
            SchedulePreviewRoutingRefresh();
        }

        if (AutomationAutoTakeEnabled)
        {
            AutomationLastAction = $"Taking {RecommendedSceneName} to program";
            _ = TakeAutomationPreviewAsync(targetSceneId);
        }
        else
        {
            AutomationLastAction = $"{RecommendedSceneName} queued on preview";
            CommandStatus = AutomationLastAction;
        }
    }

    private async Task TakeAutomationPreviewAsync(string targetSceneId)
    {
        if (_automationTakeInFlight || !string.Equals(PreviewSceneId, targetSceneId, StringComparison.Ordinal) || !CanTake)
        {
            return;
        }

        _automationTakeInFlight = true;
        try
        {
            await TakeAsync();
            _automationPendingSceneId = null;
            _automationPendingSince = null;
            AutomationLastAction = $"{Scenes.First(scene => scene.Id == targetSceneId).Name} taken by automation";
        }
        finally
        {
            _automationTakeInFlight = false;
        }
    }

    private void ApplyAutomationOverlayPolicy()
    {
        var changed = false;
        changed |= SetAutomationGraphic("lower-third", AutomationLowerThirdsEnabled);
        changed |= SetAutomationGraphic("caption", AutomationCaptionsEnabled);

        RefreshProgramLowerThirdKeyPosition();

        if (!AutomationCaptionsEnabled)
        {
            if (!string.IsNullOrEmpty(CaptionText))
            {
                CaptionText = string.Empty;
                changed = true;
            }

            if (!string.IsNullOrEmpty(CaptionSpeaker))
            {
                CaptionSpeaker = string.Empty;
                changed = true;
            }
        }

        if (changed)
        {
            OnPropertyChanged(nameof(EnabledGraphics));
            _ = TrySyncMediaCoreAsync();
        }
    }

    private bool SetAutomationGraphic(string kind, bool enabled)
    {
        var automationGraphicId = $"automation-{kind}";
        var graphic = Graphics.FirstOrDefault(item => string.Equals(item.Id, automationGraphicId, StringComparison.Ordinal));
        if (graphic is null && enabled)
        {
            Graphics.Add(new GraphicOverlay
            {
                Id = automationGraphicId,
                Name = kind == "caption" ? "Caption strip" : "Lower third",
                Kind = kind,
                Position = kind == "caption" ? "bottom" : Overlays.LowerThirdPosition,
                Accent = BrandKit.AccentColor,
                Enabled = true
            });
            return true;
        }

        if (graphic is not null && graphic.Enabled != enabled)
        {
            graphic.Enabled = enabled;
            return true;
        }

        return false;
    }

    public bool HasCaptureDevices => CaptureDevices.Count > 0;

    public bool HasMediaBinAssets => MediaBinGroups.Sum(group => group.Assets.Count) > 0;

    public bool HasSelectedMediaAsset => !string.IsNullOrWhiteSpace(SelectedMediaAssetId);

    public string SelectedMediaAssetSummary =>
        HasSelectedMediaAsset
            ? $"{SelectedMediaAssetName} selected"
            : "No media asset selected";

    public string MediaPlaybackButtonLabel => SelectedMediaAssetPlaying ? "Pause" : "Play";

    public string NativeMediaPlaybackStatus =>
        _bridge.LastSnapshot?.MediaPlayback is { } playback
            ? FormatNativeMediaPlaybackStatus(playback)
            : "Native media playback waiting for media core.";

    public bool CanAddSelectedMediaAssetToPreview => HasSelectedMediaAsset;

    public bool HasCaptionTranscript => CaptionTranscript.Count > 0;

    public string CaptionTranscriptEmptyGuidance =>
        "Caption lines appear here when the engine is live and the caption track publishes cues.";

    [RelayCommand]
    private void RefreshMediaBin()
    {
        MediaBinGroups = ApplyMediaSelection(_mediaBinService.LoadGroups());
        if (SelectedMediaAssetId is not null && FindMediaAsset(SelectedMediaAssetId) is null)
        {
            SelectedMediaAssetId = null;
            SelectedMediaAssetName = null;
            SelectedMediaAssetPath = null;
            SelectedMediaAssetKind = null;
            SelectedMediaAssetPlaying = false;
            MediaPlaybackStatus = "No media asset playing";
        }

        MediaBinGuidance = MediaBinClassifier.BuildEmptyGuidanceMessage();
        OnPropertyChanged(nameof(MediaBinGroups));
        OnPropertyChanged(nameof(HasMediaBinAssets));
        OnPropertyChanged(nameof(HasSelectedMediaAsset));
        OnPropertyChanged(nameof(SelectedMediaAssetSummary));
        OnPropertyChanged(nameof(MediaPlaybackButtonLabel));
        OnPropertyChanged(nameof(CanAddSelectedMediaAssetToPreview));
        OnPropertyChanged(nameof(SuperSourceBackgroundOptions));
        PruneMissingSceneBackgrounds();
        RefreshSceneBackgroundSelection();
        SyncPreviewCanvasLayers(GetPreviewEditableRoutes());
        RefreshMultiviewGridTiles();
        RefreshProductionReadouts();
    }

    [RelayCommand]
    private async Task ImportMediaAssetsAsync()
    {
        var picker = new FileOpenPicker
        {
            SuggestedStartLocation = PickerLocationId.VideosLibrary,
            ViewMode = PickerViewMode.List
        };
        foreach (var extension in _mediaBinService.SupportedExtensions)
        {
            picker.FileTypeFilter.Add(extension);
        }

        var hwnd = App.MainWindowHandle;
        if (hwnd != IntPtr.Zero)
        {
            InitializeWithWindow.Initialize(picker, hwnd);
        }

        var files = await picker.PickMultipleFilesAsync();
        if (files.Count == 0)
        {
            CommandStatus = "Media import canceled";
            return;
        }

        var imported = _mediaBinService.ImportFiles(files.Select(file => file.Path));
        RefreshMediaBin();
        var firstImported = imported.FirstOrDefault();
        if (firstImported is not null)
        {
            SelectMediaAsset(firstImported.Id);
            CommandStatus = imported.Count == 1
                ? $"{firstImported.Name} imported and selected"
                : $"{imported.Count} media assets imported";
            return;
        }

        CommandStatus = "No supported media files imported";
    }

    [RelayCommand]
    private async Task BrowseFfmpegBinFolderAsync()
    {
        var picker = new FolderPicker
        {
            SuggestedStartLocation = PickerLocationId.ComputerFolder
        };
        picker.FileTypeFilter.Add("*");

        var hwnd = App.MainWindowHandle;
        if (hwnd != IntPtr.Zero)
        {
            InitializeWithWindow.Initialize(picker, hwnd);
        }

        var folder = await picker.PickSingleFolderAsync();
        if (folder is null)
        {
            CommandStatus = "FFmpeg folder selection canceled";
            return;
        }

        FfmpegBinDirectory = folder.Path;
        CommandStatus = FfmpegRuntimeStatus;
    }

    private static void ApplyFfmpegRuntimeEnvironment(string? binDirectory)
    {
        var normalized = string.IsNullOrWhiteSpace(binDirectory)
            ? string.Empty
            : Path.GetFullPath(binDirectory.Trim());
        Environment.SetEnvironmentVariable("COREVIDEO_FFMPEG_BIN_DIR", normalized, EnvironmentVariableTarget.Process);
        if (!Directory.Exists(normalized))
        {
            return;
        }

        Environment.SetEnvironmentVariable("COREVIDEO_FFMPEG_BIN_DIR", normalized, EnvironmentVariableTarget.User);
        var path = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
        var entries = path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (entries.Any(entry => PathsEqual(entry, normalized)))
        {
            return;
        }

        Environment.SetEnvironmentVariable("PATH", string.Join(Path.PathSeparator, entries.Prepend(normalized)));
    }

    private static bool PathsEqual(string left, string right)
    {
        try
        {
            return string.Equals(Path.GetFullPath(left), Path.GetFullPath(right), StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception)
        {
            return string.Equals(left, right, StringComparison.OrdinalIgnoreCase);
        }
    }

    [RelayCommand]
    private void SelectMediaAsset(string assetId)
    {
        var asset = FindMediaAsset(assetId);
        if (asset is null)
        {
            return;
        }

        SelectedMediaAssetId = asset.Id;
        SelectedMediaAssetName = asset.Name;
        SelectedMediaAssetPath = asset.FilePath;
        SelectedMediaAssetKind = asset.Kind;
        SelectedMediaAssetPlaying = false;
        MediaPlaybackStatus = $"{asset.Name} selected";
        MediaBinGroups = ApplyMediaSelection(MediaBinGroups);
        OnPropertyChanged(nameof(MediaBinGroups));
        OnPropertyChanged(nameof(HasSelectedMediaAsset));
        OnPropertyChanged(nameof(SelectedMediaAssetSummary));
        OnPropertyChanged(nameof(MediaPlaybackButtonLabel));
        OnPropertyChanged(nameof(CanAddSelectedMediaAssetToPreview));
        RefreshMultiviewGridTiles();
        CommandStatus = $"{asset.Name} ready for playback";
        _ = TrySyncMediaCoreAsync();
    }

    // Lifecycle L4 (source-lifecycle-spec): remove a media asset from the bin.
    // If a scene layer or Show Input references it, we REFUSE with an honest
    // message naming the references (no silent no-ops; the operator unassigns
    // first). Otherwise: stop playback if it is the playing asset, move the
    // file to the recoverable .trash, clear selection, refresh.
    [RelayCommand]
    private void RemoveMediaAsset(string assetId)
    {
        var asset = FindMediaAsset(assetId);
        if (asset is null)
        {
            return;
        }

        var references = FindMediaAssetReferences(asset.Id);
        if (references.Count > 0)
        {
            CommandStatus = $"'{asset.Name}' is in use ({string.Join(", ", references)}). Unassign it there before removing.";
            LaunchLog.Write($"lifecycle: media remove blocked {asset.Id} - referenced by {string.Join("; ", references)}");
            return;
        }

        if (IsMediaAssetPlaying(asset.Id))
        {
            SelectedMediaAssetPlaying = false;
            MediaPlaybackStatus = "No media asset playing";
        }

        var removed = _mediaBinService.DeleteAsset(asset);
        if (!removed)
        {
            CommandStatus = $"Could not remove '{asset.Name}'.";
            return;
        }

        LaunchLog.Write($"lifecycle: media removed {asset.Id} ({asset.Name})");
        if (string.Equals(SelectedMediaAssetId, asset.Id, StringComparison.Ordinal))
        {
            SelectedMediaAssetId = null;
            SelectedMediaAssetName = string.Empty;
            SelectedMediaAssetPath = string.Empty;
        }
        RefreshMediaBin();
        OnPropertyChanged(nameof(HasSelectedMediaAsset));
        OnPropertyChanged(nameof(SelectedMediaAssetSummary));
        CommandStatus = $"Removed '{asset.Name}' (recoverable from the media .trash folder).";
    }

    // Scene layers and Show Input slots that reference a media asset, as
    // human-readable labels for the refuse-when-in-use message.
    private IReadOnlyList<string> FindMediaAssetReferences(string assetId)
    {
        var references = new List<string>();
        foreach (var (sceneId, routes) in _sceneRoutes)
        {
            var uses = routes.Any(route =>
                ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var id) &&
                string.Equals(id, assetId, StringComparison.Ordinal));
            if (uses)
            {
                var sceneName = _scenes.FirstOrDefault(s => string.Equals(s.Id, sceneId, StringComparison.Ordinal))?.Name ?? sceneId;
                references.Add($"scene '{sceneName}'");
            }
        }
        foreach (var slot in ShowInputs)
        {
            if (ShowInputRosterService.TryGetMediaAssetId(slot.ParticipantId, out var id) &&
                string.Equals(id, assetId, StringComparison.Ordinal))
            {
                references.Add($"Input {slot.SlotNumber}");
            }
        }
        return references;
    }

    public void UseSelectedMediaAssetAsBrandLogo()
    {
        if (string.IsNullOrWhiteSpace(SelectedMediaAssetId))
        {
            CommandStatus = "Select a media asset before assigning a brand logo";
            return;
        }

        var asset = FindMediaAsset(SelectedMediaAssetId);
        if (asset is null)
        {
            CommandStatus = "Selected media asset is no longer available";
            return;
        }

        BrandKit = new BrandKit
        {
            Name = BrandKit.Name,
            LogoText = BrandKit.LogoText,
            LogoAssetId = asset.Id,
            LogoAssetName = asset.Name,
            LogoAssetPath = asset.FilePath,
            BrandColor = BrandKit.BrandColor,
            AccentColor = BrandKit.AccentColor,
            BackgroundColor = BrandKit.BackgroundColor,
            FontFamily = BrandKit.FontFamily,
            LowerThirdStyle = BrandKit.LowerThirdStyle,
            CaptionStyle = BrandKit.CaptionStyle,
            DefaultOverlayBehavior = BrandKit.DefaultOverlayBehavior
        };
        Overlays.NotifyBrandKitChanged();
        CommandStatus = $"{asset.Name} assigned as brand logo";
        _ = TrySyncMediaCoreAsync();
    }

    public bool IsMediaAssetPlaying(string assetId) =>
        SelectedMediaAssetPlaying &&
        string.Equals(SelectedMediaAssetId, assetId, StringComparison.Ordinal);

    private void PromoteProgramMediaRouteToPlayback()
    {
        var programRoutes = GetResolvedProgramRoutes();
        var mediaAssetId = MediaRoutePlaybackService.ResolveProgramAutoplayAssetId(
            SelectedMediaAssetId,
            programRoutes);
        if (string.IsNullOrWhiteSpace(mediaAssetId) ||
            FindMediaAsset(mediaAssetId) is not { } asset)
        {
            return;
        }

        SelectedMediaAssetId = asset.Id;
        SelectedMediaAssetName = asset.Name;
        SelectedMediaAssetPath = asset.FilePath;
        SelectedMediaAssetKind = asset.Kind;
        SelectedMediaAssetPlaying = true;
        MediaPlaybackStatus = $"Playing {asset.Name} on Program";
        MediaBinGroups = ApplyMediaSelection(MediaBinGroups);

        OnPropertyChanged(nameof(MediaBinGroups));
        OnPropertyChanged(nameof(HasSelectedMediaAsset));
        OnPropertyChanged(nameof(SelectedMediaAssetSummary));
        OnPropertyChanged(nameof(MediaPlaybackButtonLabel));
        OnPropertyChanged(nameof(CanAddSelectedMediaAssetToPreview));
        OnPropertyChanged(nameof(IsMediaAssetPlaying));
        OnPropertyChanged(nameof(MediaPlaybackStatus));
        RefreshMultiviewGridTiles();
    }

    [RelayCommand]
    private void PlayMediaAsset(string assetId)
    {
        if (string.IsNullOrWhiteSpace(assetId))
        {
            return;
        }

        var asset = FindMediaAsset(assetId);
        if (asset is null)
        {
            return;
        }

        // Re-pressing the asset that is already playing pauses it; otherwise select and play.
        var resumeSameAsset = string.Equals(SelectedMediaAssetId, assetId, StringComparison.Ordinal);
        SelectedMediaAssetId = asset.Id;
        SelectedMediaAssetName = asset.Name;
        SelectedMediaAssetPath = asset.FilePath;
        SelectedMediaAssetKind = asset.Kind;
        SelectedMediaAssetPlaying = !(resumeSameAsset && SelectedMediaAssetPlaying);
        if (MediaRoutePlaybackService.ShouldAdvanceProgramPlaybackKey(
                asset.Id,
                SelectedMediaAssetPlaying,
                GetResolvedProgramRoutes()))
        {
            _programMediaPlaybackTakeVersion++;
        }

        MediaBinGroups = ApplyMediaSelection(MediaBinGroups);

        MediaPlaybackStatus = SelectedMediaAssetPlaying
            ? $"Playing {asset.Name}"
            : $"{asset.Name} paused";
        CommandStatus = MediaPlaybackStatus;

        OnPropertyChanged(nameof(MediaBinGroups));
        OnPropertyChanged(nameof(HasSelectedMediaAsset));
        OnPropertyChanged(nameof(SelectedMediaAssetSummary));
        OnPropertyChanged(nameof(MediaPlaybackButtonLabel));
        OnPropertyChanged(nameof(CanAddSelectedMediaAssetToPreview));
        OnPropertyChanged(nameof(IsMediaAssetPlaying));
        RefreshMultiviewGridTiles();

        // Push the selection through the typed boundary the same way scene takes do.
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private void ToggleSelectedMediaPlayback()
    {
        if (string.IsNullOrWhiteSpace(SelectedMediaAssetId))
        {
            CommandStatus = "Select a media asset before playback";
            return;
        }

        PlayMediaAsset(SelectedMediaAssetId);
    }

    private bool _applyingProductionPatch;
    private int _productionSyncRetryQueued;

    private async Task TrySyncMediaCoreAsync()
    {
        // Don't re-sync in response to a sync's OWN snapshot patch. ApplyLiveProductionPatch
        // applies the snapshot to bound state, which (on a structural change) rebuilds the
        // roster/multiview/show-input collections; some of those paths call TrySyncMediaCoreAsync,
        // which fed a self-sustaining production-sync loop (~13/s back-to-back, each running the
        // full media-core tick ~60ms and starving the render → frozen preview). The bridge's 4s
        // timeouts previously masked it; with the JSON-parse fix the syncs now complete and the
        // loop ran at full speed.
        if (_applyingProductionPatch)
        {
            return;
        }

        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(false);
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (MediaCoreSyncInFlightException)
        {
            QueueProductionSyncRetry("sync-in-flight");
        }
        catch (Exception ex)
        {
            CommandStatus = ex.Message;
        }
    }

    private void QueueProductionSyncRetry(string reason)
    {
        if (Interlocked.Exchange(ref _productionSyncRetryQueued, 1) == 1)
        {
            return;
        }

        LaunchLog.Write($"media-core sync deferred reason={reason}; retry queued");
        _ = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(350).ConfigureAwait(false);
                if (_bridge.Running)
                {
                    await TrySyncMediaCoreAsync().ConfigureAwait(false);
                }
            }
            finally
            {
                Interlocked.Exchange(ref _productionSyncRetryQueued, 0);
            }
        });
    }

    private async Task StartMediaCoreOnLaunchAsync()
    {
        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(false);
            await SyncActiveSceneAsync().ConfigureAwait(false);
            // Re-apply the saved multiviewer preferences so the core matches the operator's choice.
            await ConfigureMultiviewerAsync().ConfigureAwait(false);
            RunOnUiThread(() =>
            {
                EngineStatus = $"Media core ready - {_bridge.ProfileSummary}";
                RefreshSurfaceBindings();
                RefreshOutputStatus();
                RefreshTransportState();
            });
        }
        catch (Exception ex)
        {
            RunOnUiThread(() =>
            {
                EngineStatus = $"Media core unavailable - {ex.Message}";
                CommandStatus = EngineStatus;
                RefreshTransportState();
            });
        }
    }

    private async Task EnsureMediaCoreRunningAsync(string startingStatus)
    {
        if (_bridge.Running)
        {
            return;
        }

        RunOnUiThread(() => EngineStatus = startingStatus);
        await _bridge.StartAsync().ConfigureAwait(false);
        RunOnUiThread(() =>
        {
            Settings.RefreshSdkReadiness();
            OnPropertyChanged(nameof(CanToggleRecording));
            ToggleRecordingCommand.NotifyCanExecuteChanged();
        });
    }

    [RelayCommand]
    private void RefreshCaptureDevices() => _ = RefreshCaptureDevicesAsync();

    private async Task RefreshAudioCaptureDevicesAsync()
    {
        IReadOnlyList<AudioCaptureDevice> discovered;
        try
        {
            discovered = await _audioCaptureDiscovery.DiscoverDevicesAsync().ConfigureAwait(false);
        }
        catch
        {
            discovered = [];
        }

        RunOnUiThread(() => ApplyDiscoveredAudioCaptureDevices(discovered));
    }

    private void ApplyDiscoveredAudioCaptureDevices(IReadOnlyList<AudioCaptureDevice> discovered)
    {
        _lastDiscoveredAudioCaptureDevices = discovered;
        RebuildAudioCaptureDeviceCatalog();
    }

    private void RebuildAudioCaptureDeviceCatalog()
    {
        var selectedAudioId = SelectedLocalAudioCaptureDeviceId;
        var catalog = _lastDiscoveredAudioCaptureDevices
            .Concat(AudioCaptureDeviceDiscoveryService.CreateEmbeddedCaptureAudioDevices(CaptureDevices))
            .GroupBy(device => device.Id, StringComparer.OrdinalIgnoreCase)
            .Select(group => group.First())
            .OrderBy(device => AudioCaptureDeviceDiscoveryService.SourceKindPriority(device.SourceKind))
            .ThenBy(device => device.IsDefault ? 0 : 1)
            .ThenBy(device => device.DriverName, StringComparer.OrdinalIgnoreCase)
            .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

        AudioCaptureDevices.Clear();
        foreach (var device in catalog)
        {
            AudioCaptureDevices.Add(device);
        }

        NormalizeCaptureAudioAssignments();
        if (string.IsNullOrWhiteSpace(selectedAudioId))
        {
            SelectedLocalAudioCaptureDeviceId = ResolveLocalAudioSourceDeviceId(selectedAudioId, AudioCaptureDevices);
        }
        else if (!string.Equals(SelectedLocalAudioCaptureDeviceId, selectedAudioId, StringComparison.Ordinal))
        {
            SelectedLocalAudioCaptureDeviceId = selectedAudioId;
        }
        // A saved id MISSING from the catalog stays selected: the first rebuild
        // runs before WASAPI discovery returns, and resolving-to-default here
        // overwrote the persisted mic choice on every launch (owner: Chat Mic
        // reverted to system loopback after each restart).

        RefreshShowInputEditors();
        OnPropertyChanged(nameof(AudioCaptureDevices));
        OnPropertyChanged(nameof(AudioCaptureSourceOptions));
        BuildAudioRoutingMatrix();
        RefreshAudioParticipantRows();
        RefreshLocalAudioSourceBindings();
        _ = TrySyncMediaCoreAsync();
    }

    private void NormalizeCaptureAudioAssignments()
    {
        var audioById = AudioCaptureDevices.ToDictionary(device => device.Id, StringComparer.Ordinal);
        foreach (var captureDevice in CaptureDevices)
        {
            var audioDevice = ResolveCaptureAudioAssignment(captureDevice, audioById, AudioCaptureDevices);
            if (audioDevice is null)
            {
                captureDevice.AssignedAudioDeviceId = null;
                captureDevice.AssignedAudioDeviceName = null;
            }
            else
            {
                captureDevice.AssignedAudioDeviceName = audioDevice.DisplayLabel;
            }

            captureDevice.NotifyAudioAssignmentChanged();
        }
    }

    public static AudioCaptureDevice? ResolveCaptureAudioAssignment(
        CaptureDevice captureDevice,
        IReadOnlyDictionary<string, AudioCaptureDevice> audioById,
        IEnumerable<AudioCaptureDevice> audioDevices)
    {
        if (!string.IsNullOrWhiteSpace(captureDevice.AssignedAudioDeviceId) &&
            audioById.TryGetValue(captureDevice.AssignedAudioDeviceId, out var assignedAudio))
        {
            return assignedAudio;
        }

        return audioDevices.FirstOrDefault(device =>
            device.IsAvailable &&
            device.IsEmbeddedCaptureAudio &&
            string.Equals(device.LinkedCaptureDeviceId, captureDevice.Id, StringComparison.Ordinal));
    }

    private async Task RefreshAudioRenderDevicesAsync()
    {
        IReadOnlyList<AudioRenderDevice> discovered;
        try
        {
            discovered = await _audioRenderDiscovery.DiscoverDevicesAsync().ConfigureAwait(false);
        }
        catch
        {
            discovered = [];
        }

        RunOnUiThread(() => ApplyDiscoveredAudioRenderDevices(discovered));
    }

    private void ApplyDiscoveredAudioRenderDevices(IReadOnlyList<AudioRenderDevice> discovered)
    {
        AudioRenderDevices.Clear();
        foreach (var device in discovered)
        {
            AudioRenderDevices.Add(device);
        }

        if (string.IsNullOrWhiteSpace(SelectedAudioMonitorDeviceId) ||
            AudioRenderDevices.All(device => !string.Equals(device.Id, SelectedAudioMonitorDeviceId, StringComparison.Ordinal)))
        {
            // First run, or the saved device disappeared: land on the OS default
            // (the "System default output" pseudo-entry), never on whichever
            // device happens to sort first alphabetically (phase B2).
            SelectedAudioMonitorDeviceId =
                AudioRenderDevices.FirstOrDefault(device => device.IsDefault)?.Id ??
                AudioRenderDevices.FirstOrDefault()?.Id ?? string.Empty;
        }

        OnPropertyChanged(nameof(AudioRenderDevices));
        RefreshAudioMonitorBindings();
    }

    public void SetCaptureDeviceAudioSource(string? captureDeviceId, string? audioDeviceId)
    {
        if (string.IsNullOrWhiteSpace(captureDeviceId) ||
            CaptureDevices.FirstOrDefault(device =>
                string.Equals(device.Id, captureDeviceId, StringComparison.Ordinal)) is not { } captureDevice)
        {
            return;
        }

        var normalizedAudioDeviceId = string.IsNullOrWhiteSpace(audioDeviceId) ? null : audioDeviceId;
        var audioDevice = normalizedAudioDeviceId is null
            ? null
            : AudioCaptureDevices.FirstOrDefault(device =>
                string.Equals(device.Id, normalizedAudioDeviceId, StringComparison.Ordinal));

        captureDevice.AssignedAudioDeviceId = audioDevice?.Id;
        captureDevice.AssignedAudioDeviceName = audioDevice?.DisplayLabel;
        captureDevice.NotifyAudioAssignmentChanged();

        foreach (var slot in ShowInputs.Where(slot =>
            string.Equals(slot.CaptureDeviceId, captureDevice.Id, StringComparison.Ordinal)))
        {
            slot.AudioDeviceId = captureDevice.AssignedAudioDeviceId;
        }

        RefreshShowInputEditors();
        BuildAudioRoutingMatrix();
        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
    }

    [RelayCommand]
    private async Task ConnectCaptureDeviceAsync(string deviceId)
    {
        var device = CaptureDevices.FirstOrDefault(d => d.Id == deviceId);
        if (device is null || device.ConnectionState == CaptureConnectionState.Connected)
        {
            return;
        }

        if (IsVirtualSrtIngestDevice(device))
        {
            device.ConnectionState = CaptureConnectionState.Connected;
            device.SignalPresent = false;
            AssignConnectedCaptureDeviceToShowInput(device);
            RefreshDualCaptureSourceOptions();
            RefreshCaptureFleetSummary();
            RefreshShowInputEditors();
            RefreshPreviewRoutingState();
            RefreshMultiviewGridTiles();
            CommandStatus = "SRT source routed. Waiting for video.";
            return;
        }

        // Browser sources (BR-1) are ALWAYS core-owned: the core spawns/supervises
        // the WebView2 host the moment browser-add lands, so there is nothing to
        // connect shell-side — the periodic native-device refresh carries their
        // real state. Reflect the core's ownership and bail.
        if (device.Id.StartsWith("browser:", StringComparison.Ordinal))
        {
            device.ConnectionState = CaptureConnectionState.Connected;
            RefreshCaptureFleetSummary();
            RefreshShowInputEditors();
            return;
        }

        // Screen sources connect CORE-SIDE: connect-capture-device starts the
        // WGC session and frames enter the compositor as "capture:screen:<n>".
        if (device.Id.StartsWith("screen:", StringComparison.Ordinal) || device.Id.StartsWith("window:", StringComparison.Ordinal))
        {
            try
            {
                LaunchLog.Write(string.Format("capture: screen connect requested {0}", device.Id));
                var statuses = await _bridge.ConnectNativeCaptureDeviceAsync(device.Id).ConfigureAwait(false);
                var match = statuses.FirstOrDefault(s => s.Id == device.Id);
                LaunchLog.Write(string.Format("capture: screen connect response {0}: statuses={1} match={2}", device.Id, statuses.Count, match?.ConnectionState ?? "none"));
                RunOnUiThread(() =>
                {
                    device.ConnectionState = match?.ConnectionState == "connected"
                        ? CaptureConnectionState.Connected
                        : CaptureConnectionState.Detected;
                    device.SignalPresent = match?.SignalPresent ?? false;
                    if (device.ConnectionState == CaptureConnectionState.Connected)
                    {
                        AssignConnectedCaptureDeviceToShowInput(device);
                        RefreshDualCaptureSourceOptions();
                        RefreshCaptureFleetSummary();
                        RefreshShowInputEditors();
                        RefreshPreviewRoutingState();
                        RefreshMultiviewGridTiles();
                        CommandStatus = $"Screen capture live: {device.Name}.";
                    }
                    else
                    {
                        CommandStatus = $"Screen capture failed to start for {device.Name}.";
                    }
                });
            }
            catch (Exception ex)
            {
                LaunchLog.Write($"capture: screen connect failed {device.Id}: {ex.Message}");
            }
            return;
        }

        // Native-first UVC path (opt-in via COREVIDEO_NATIVE_UVC=1): let the
        // core open the camera with its Media Foundation adapter — frames enter
        // the compositor directly, skipping the MediaCapture -> shared-memory
        // hop. Any decline (stub core, unknown device, open failure) falls
        // through to the existing WinUI MediaCapture bridge below.
        if (NativeUvcCapturePolicy.ShouldPreferNativeCapture(
                Environment.GetEnvironmentVariable(NativeUvcCapturePolicy.EnvironmentVariableName),
                device.Vendor))
        {
            if (await TryConnectNativeUvcCaptureAsync(device).ConfigureAwait(false))
            {
                return;
            }

            LaunchLog.Write($"capture: native uvc declined {device.Id}; falling back to WinUI MediaCapture bridge");
        }

        try
        {
            var format = await _captureFrameReader.StartAsync(device).ConfigureAwait(false);
            RunOnUiThread(() =>
            {
                device.ConnectionState = CaptureConnectionState.Connected;
                device.ApplyFormatTelemetry(format.Width, format.Height, format.Fps);
                device.SignalPresent = false;
                AssignConnectedCaptureDeviceToShowInput(device);
                RefreshDualCaptureSourceOptions();
                RefreshCaptureFleetSummary();
                RefreshShowInputEditors();
                RefreshPreviewRoutingState();
                RefreshMultiviewGridTiles();
                CommandStatus = $"{device.Name} brought online as program source";
            });
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"capture: open failed {device.Id}: {ex.GetType().Name}: {ex.Message}");
            RunOnUiThread(() =>
            {
                device.ConnectionState = CaptureConnectionState.Error;
                device.SignalPresent = false;
                RefreshCaptureFleetSummary();
                RefreshShowInputEditors();
                RefreshPreviewRoutingState();
                RefreshMultiviewGridTiles();
                CommandStatus = $"{device.Name} failed to open: {ex.Message}";
            });
        }
    }

    // Attempts the native core (Media Foundation UVC) capture path for a camera.
    // Success = the core's connect-capture-device response reports this device
    // connected (matched by stable id, or by the OS symbolic link when the
    // hashed ids disagree over symbolic-link casing). Never throws.
    private async Task<bool> TryConnectNativeUvcCaptureAsync(CaptureDevice device)
    {
        try
        {
            // outputSourceId = device.Id: the core keys the camera's frames by the
            // SHELL's routing id, not its own MF-enumerated id, so the native frame
            // (`capture:<device.Id>`) matches the multiview/scene routing instead of
            // showing a placeholder tile.
            var devices = await _bridge.ConnectNativeCaptureDeviceAsync(device.Id, outputSourceId: device.Id).ConfigureAwait(false);
            var match = NativeUvcCapturePolicy.FindConnectedDevice(devices, device.Id, device.NativeDeviceId);
            if (match is null && !string.IsNullOrWhiteSpace(device.NativeDeviceId))
            {
                // Stable-id mismatch (WinRT vs MF symbolic-link casing): retry
                // addressing the device by its OS identity, which the core
                // matches case-insensitively — but STILL key frames by device.Id.
                devices = await _bridge.ConnectNativeCaptureDeviceAsync(device.NativeDeviceId, outputSourceId: device.Id).ConfigureAwait(false);
                match = NativeUvcCapturePolicy.FindConnectedDevice(devices, device.Id, device.NativeDeviceId);
            }

            if (match is null)
            {
                return false;
            }

            // The core now owns this camera via Media Foundation. Tear down any managed
            // MediaCapture bridge session for the same device so we never run two readers
            // against one device (the bridge reader would otherwise keep restarting — see
            // the OutputFormatNotSupported storm on 2026-07-10). No-op if none is running.
            await _captureFrameReader.StopAsync(device.Id).ConfigureAwait(false);

            LaunchLog.Write(
                $"capture: native uvc connected {device.Id} ({match.Width}x{match.Height}@{match.FrameRate}, core id {match.Id})");
            RunOnUiThread(() =>
            {
                device.ConnectionState = CaptureConnectionState.Connected;
                device.ApplyFormatTelemetry(match.Width, match.Height, match.FrameRate);
                device.SignalPresent = match.SignalPresent;
                AssignConnectedCaptureDeviceToShowInput(device);
                RefreshDualCaptureSourceOptions();
                RefreshCaptureFleetSummary();
                RefreshShowInputEditors();
                RefreshPreviewRoutingState();
                RefreshMultiviewGridTiles();
                CommandStatus = $"{device.Name} brought online via native core capture";
            });
            return true;
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"capture: native uvc connect failed {device.Id}: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
    }

    [RelayCommand]
    private Task NudgeCaptureAudioEarlierAsync(string deviceId) =>
        SetCaptureAudioSyncOffsetAsync(deviceId, ResolveCaptureAudioSyncOffset(deviceId) - 20);

    [RelayCommand]
    private Task NudgeCaptureAudioLaterAsync(string deviceId) =>
        SetCaptureAudioSyncOffsetAsync(deviceId, ResolveCaptureAudioSyncOffset(deviceId) + 20);

    [RelayCommand]
    private Task ResetCaptureAudioSyncAsync(string deviceId) =>
        SetCaptureAudioSyncOffsetAsync(deviceId, 0);

    private async Task SetCaptureAudioSyncOffsetAsync(string deviceId, int offsetMs)
    {
        var device = CaptureDevices.FirstOrDefault(item => string.Equals(item.Id, deviceId, StringComparison.Ordinal));
        if (device is null)
        {
            return;
        }

        var clamped = Math.Clamp(offsetMs, -500, 500);
        device.AudioSyncOffsetMs = clamped;
        RefreshCaptureFleetSummary();

        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(false);
            await _bridge.SetCaptureAudioSyncOffsetAsync(deviceId, clamped).ConfigureAwait(false);
            RunOnUiThread(() => CommandStatus = $"{device.Name} audio sync set to {clamped} ms");
        }
        catch (Exception ex)
        {
            RunOnUiThread(() => CommandStatus = $"{device.Name} audio sync not applied: {ex.Message}");
        }
    }

    private int ResolveCaptureAudioSyncOffset(string deviceId) =>
        CaptureDevices.FirstOrDefault(item => string.Equals(item.Id, deviceId, StringComparison.Ordinal))?.AudioSyncOffsetMs ?? 0;

    [RelayCommand]
    private void ToggleSelectedParticipantMute()
    {
        var mix = SelectedAudioMix;
        if (mix is null)
        {
            return;
        }

        mix.Muted = !mix.Muted;
        RefreshMixerValueBindings(mix.ParticipantId);
        RefreshAudioProcessingTargets();
        OnPropertyChanged(nameof(SelectedMuteButtonLabel));
        OnPropertyChanged(nameof(SelectedInsertChainLabel));
        CommandStatus = mix.Muted
            ? $"{SelectedParticipant?.Name} muted in mix"
            : $"{SelectedParticipant?.Name} unmuted in mix";
        _ = TrySyncMediaCoreAsync();
    }

    private void RefreshCaptureFleetSummary()
    {
        CaptureFleetSummary = ProductionStateHelper.CaptureFleetSummary(CaptureDevices);
        UpdateDualCaptureSummary();
    }

    private void OnSrtIngestSettingsChanged()
    {
        OnPropertyChanged(nameof(SrtIngestSummary));
        OnPropertyChanged(nameof(SrtIngestCountSummary));
        OnPropertyChanged(nameof(CanAddSrtIngestSource));
        OnPropertyChanged(nameof(SrtIngestRuntimeSummary));
        RefreshVirtualSrtIngestDevices();
        RefreshShowInputEditors();
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
    }

    // ---- Browser sources (BR-1, docs/capture-sources-spec.md §4) ------------------
    // Minimal add affordance: URL + size preset. The CORE owns the WebView2 host
    // process; the source arrives via the periodic native-device refresh as a
    // "browser:<n>" capture device (kind chip "Browser" in the unified picker).

    [ObservableProperty]
    private string _newBrowserSourceUrl = string.Empty;

    [ObservableProperty]
    private int _newBrowserSourcePresetIndex;

    public IReadOnlyList<string> BrowserSourcePresetOptions { get; } =
    [
        "1920x1080 (full-canvas overlay)",
        "1280x720",
        "960x540 (widget)"
    ];

    private static readonly (int Width, int Height)[] BrowserSourcePresets =
    [
        (1920, 1080),
        (1280, 720),
        (960, 540)
    ];

    [RelayCommand]
    private async Task AddBrowserSourceAsync()
    {
        var url = NewBrowserSourceUrl?.Trim() ?? string.Empty;
        if (url.Length == 0)
        {
            CommandStatus = "Enter a URL for the browser source (https://…).";
            return;
        }
        if (!url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) &&
            !url.StartsWith("https://", StringComparison.OrdinalIgnoreCase) &&
            !url.StartsWith("file://", StringComparison.OrdinalIgnoreCase) &&
            !url.StartsWith("data:", StringComparison.OrdinalIgnoreCase))
        {
            url = "https://" + url;  // operator typed a bare host — assume https
        }

        var presetIndex = Math.Clamp(NewBrowserSourcePresetIndex, 0, BrowserSourcePresets.Length - 1);
        var (width, height) = BrowserSourcePresets[presetIndex];
        try
        {
            await _bridge.AddBrowserSourceAsync(url, width, height, 30).ConfigureAwait(false);
            RunOnUiThread(() =>
            {
                NewBrowserSourceUrl = string.Empty;
                CommandStatus = $"Browser source added ({width}x{height}) — starting its host…";
            });
            // Pick the new "browser:<n>" device up immediately (it also arrives on
            // the periodic refresh).
            await RefreshCaptureDevicesAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"browser: add failed: {ex.Message}");
            RunOnUiThread(() => CommandStatus = $"Browser source add FAILED: {ex.Message}");
        }
    }

    // Control-surface entry points (browser.remove / browser.reload). LOUD on failure.
    public async Task RemoveBrowserSourceAsync(string browserId)
    {
        try
        {
            await _bridge.RemoveBrowserSourceAsync(browserId).ConfigureAwait(false);
            RunOnUiThread(() =>
            {
                _onAirBrowserOverlayIds.Remove(browserId);
                _previewBrowserOverlayIds.Remove(browserId);
                if (string.Equals(_primaryBrowserOverlayId, browserId, StringComparison.Ordinal))
                {
                    _primaryBrowserOverlayId = null;
                }
                NotifyBrowserOverlayControlStateChanged();
                CommandStatus = $"Browser source {browserId} removed.";
            });
            await RefreshCaptureDevicesAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"browser: remove failed: {ex.Message}");
            RunOnUiThread(() => CommandStatus = $"Browser source remove FAILED: {ex.Message}");
            throw;
        }
    }

    public async Task ReloadBrowserSourceAsync(string browserId)
    {
        try
        {
            await _bridge.ReloadBrowserSourceAsync(browserId).ConfigureAwait(false);
            RunOnUiThread(() => CommandStatus = $"Browser source {browserId} reloading.");
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"browser: reload failed: {ex.Message}");
            RunOnUiThread(() => CommandStatus = $"Browser source reload FAILED: {ex.Message}");
            throw;
        }
    }

    [RelayCommand(CanExecute = nameof(CanAddSrtIngestSource))]
    private void AddSrtIngestSource()
    {
        if (!CanAddSrtIngestSource)
        {
            CommandStatus = $"SRT ingest is capped at {MaxSrtIngestSources} sources.";
            return;
        }

        var nextNumber = Enumerable.Range(1, MaxSrtIngestSources)
            .First(number => SrtIngestSources.All(source => source.Number != number));
        var source = CreateSrtIngestSource(nextNumber);
        SrtIngestSources.Add(source);
        CommandStatus = $"{source.Name} added as a routable SRT input";
    }

    [RelayCommand]
    private void RemoveSrtIngestSource(string sourceId)
    {
        if (SrtIngestSources.Count <= 1)
        {
            CommandStatus = "Keep at least one SRT ingest source configured.";
            return;
        }

        var source = SrtIngestSources.FirstOrDefault(item => string.Equals(item.Id, sourceId, StringComparison.Ordinal));
        if (source is null)
        {
            return;
        }

        SrtIngestSources.Remove(source);
        RemoveVirtualSrtIngestDevice(source.DeviceId);
        CommandStatus = $"{source.Name} removed from SRT inputs";
    }

    private void OnSrtIngestSourcesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (e.OldItems is not null)
        {
            foreach (var source in e.OldItems.OfType<SrtIngestSource>())
            {
                source.PropertyChanged -= OnSrtIngestSourcePropertyChanged;
            }
        }

        if (e.NewItems is not null)
        {
            foreach (var source in e.NewItems.OfType<SrtIngestSource>())
            {
                source.PropertyChanged += OnSrtIngestSourcePropertyChanged;
            }
        }

        AddSrtIngestSourceCommand.NotifyCanExecuteChanged();
        OnSrtIngestSettingsChanged();
    }

    private void OnSrtIngestSourcePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (sender is not SrtIngestSource source)
        {
            return;
        }

        OnPropertyChanged(nameof(SrtIngestSummary));
        RefreshVirtualSrtIngestDevice(source);
        RefreshShowInputEditors();
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
    }

    private void RefreshDualCaptureSourceOptions()
    {
        DualCaptureSourceOptions = ShowInputRosterService.BuildCaptureSourceOptions(CaptureDevices);

        _applyingDualCaptureSelection = true;
        try
        {
            var primary = ResolveCaptureDevice(PrimaryCaptureDeviceId);
            if (primary is null)
            {
                PrimaryCaptureDeviceId =
                    CaptureDevices.FirstOrDefault(device => device.IsConnected)?.Id ??
                    CaptureDevices.FirstOrDefault()?.Id;
            }

            var secondary = ResolveCaptureDevice(SecondaryCaptureDeviceId);
            if (secondary is null || string.Equals(SecondaryCaptureDeviceId, PrimaryCaptureDeviceId, StringComparison.Ordinal))
            {
                SecondaryCaptureDeviceId =
                    CaptureDevices.FirstOrDefault(device =>
                        device.IsConnected &&
                        !string.Equals(device.Id, PrimaryCaptureDeviceId, StringComparison.Ordinal))?.Id ??
                    CaptureDevices.FirstOrDefault(device =>
                        !string.Equals(device.Id, PrimaryCaptureDeviceId, StringComparison.Ordinal))?.Id;
            }
        }
        finally
        {
            _applyingDualCaptureSelection = false;
        }

        ApplyDualCaptureSelection();
    }

    private void ApplyDualCaptureSelection()
    {
        if (_applyingDualCaptureSelection)
        {
            return;
        }

        _applyingDualCaptureSelection = true;
        try
        {
            var primary = ResolveCaptureDevice(PrimaryCaptureDeviceId);
            var secondary = ResolveCaptureDevice(SecondaryCaptureDeviceId);
            if (primary is not null && secondary is not null && primary.Id == secondary.Id)
            {
                secondary = CaptureDevices.FirstOrDefault(device => device.Id != primary.Id);
                SecondaryCaptureDeviceId = secondary?.Id;
            }

            ApplyCaptureDeviceToShowInputSlot(0, primary);
            ApplyCaptureDeviceToShowInputSlot(1, secondary);
            UpdateDualCaptureSummary();
            RefreshShowInputEditors();
            RefreshMultiviewGridTiles();
            QueueSelectedCaptureDevicesOnline();
        }
        finally
        {
            _applyingDualCaptureSelection = false;
        }
    }

    private void ApplyCaptureDeviceToShowInputSlot(int slotIndex, CaptureDevice? device)
    {
        if (slotIndex < 0 || slotIndex >= ShowInputs.Count)
        {
            return;
        }

        var slot = ShowInputs[slotIndex];
        if (device is null)
        {
            if (slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam)
            {
                slot.Kind = ShowInputKind.Unassigned;
                slot.CaptureDeviceId = null;
                slot.InShow = false;
            }

            return;
        }

        slot.Kind = ResolveShowInputKind(device);
        slot.CaptureDeviceId = device.Id;
        slot.InShow = true;
    }

    private void UpdateDualCaptureSummary()
    {
        var primary = ResolveCaptureDevice(PrimaryCaptureDeviceId);
        var secondary = ResolveCaptureDevice(SecondaryCaptureDeviceId);
        DualCaptureLive = primary?.IsConnected == true && secondary?.IsConnected == true;
        DualCaptureSummary = (primary, secondary) switch
        {
            (null, null) => "Choose primary and secondary capture sources.",
            ({ } first, null) => $"{first.Name} selected as primary - choose a different secondary source.",
            (null, { } second) => $"{second.Name} selected as secondary - choose a primary source.",
            ({ } first, { } second) => $"{first.Name} primary - {second.Name} secondary"
        };
    }

    private CaptureDevice? ResolveCaptureDevice(string? deviceId) =>
        string.IsNullOrWhiteSpace(deviceId)
            ? null
            : CaptureDevices.FirstOrDefault(device => string.Equals(device.Id, deviceId, StringComparison.Ordinal));

    private async Task RefreshCaptureDevicesAsync()
    {
        IReadOnlyList<CaptureDevice> discovered;
        try
        {
            discovered = await _captureDiscovery.DiscoverDevicesAsync().ConfigureAwait(false);
        }
        catch
        {
            discovered = [];
        }

        // Screen sources (capture-sources-spec SC): the CORE enumerates monitors
        // ("screen:<n>" via Windows.Graphics.Capture); the WinUI device class
        // only sees webcams, so merge the core list here.
        var screens = new List<CaptureDevice>();
        try
        {
            var nativeDevices = await _bridge.ListNativeCaptureDevicesAsync().ConfigureAwait(false);
            foreach (var native in nativeDevices)
            {
                var isScreen = native.Id.StartsWith("screen:", StringComparison.Ordinal);
                var isWindow = native.Id.StartsWith("window:", StringComparison.Ordinal);
                // Browser sources (BR-1) are core-owned WebView2 host processes; they
                // enumerate as "browser:<n>" and merge in like screens do.
                var isBrowser = native.Id.StartsWith("browser:", StringComparison.Ordinal);
                if (!isScreen && !isWindow && !isBrowser)
                {
                    continue;
                }
                var inputLabel = isBrowser ? "Web page (WebView2)" : isWindow ? "Application window" : "Entire display";
                var inputId = isBrowser ? "browser" : isWindow ? "window" : "screen";
                screens.Add(new CaptureDevice
                {
                    Id = native.Id,
                    NativeDeviceId = native.Id,
                    Vendor = isBrowser ? "browser" : "Screen capture",
                    Name = native.Name,
                    Inputs = [new CaptureDeviceInput { Id = inputId, Label = inputLabel }],
                    SelectedInputId = inputId,
                    Width = native.Width,
                    Height = native.Height,
                    FrameRate = native.FrameRate,
                    ConnectionState = native.ConnectionState == "connected"
                        ? CaptureConnectionState.Connected
                        : isBrowser && native.ConnectionState == "failed"
                            ? CaptureConnectionState.Error
                            : CaptureConnectionState.Detected,
                    SignalPresent = native.SignalPresent,
                    AudioSyncOffsetMs = 0
                });
            }
        }
        catch
        {
            // Stub core or no native bridge: screens simply absent.
        }

        RunOnUiThread(() =>
        {
            ApplyDiscoveredCaptureDevices(screens.Count == 0 ? discovered : [.. discovered, .. screens]);
            SweepGhostShowInputAssignments();
            EnsureAssignedScreensConnected();
        });
    }

    private void ApplyDiscoveredCaptureDevices(IReadOnlyList<CaptureDevice> discovered)
    {
        var priorById = CaptureDevices.ToDictionary(device => device.Id, device => device);
        CaptureDevices.Clear();
        foreach (var device in discovered.Concat(CreateVirtualSrtIngestDevices()))
        {
            if (priorById.TryGetValue(device.Id, out var prior))
            {
                device.ConnectionState = prior.ConnectionState;
                device.SignalPresent = prior.SignalPresent;
                device.SelectedInputId = prior.SelectedInputId;
                device.AudioSyncOffsetMs = prior.AudioSyncOffsetMs;
                device.AssignedAudioDeviceId = prior.AssignedAudioDeviceId;
                device.AssignedAudioDeviceName = prior.AssignedAudioDeviceName;
                device.ApplyFormatTelemetry(
                    prior.Width > 0 ? prior.Width : prior.ObservedFrameWidth,
                    prior.Height > 0 ? prior.Height : prior.ObservedFrameHeight,
                    prior.FrameRate > 0 ? prior.FrameRate : prior.ObservedFrameRate);
                device.ApplyObservedFrameTelemetry(prior.ObservedFrameWidth, prior.ObservedFrameHeight, prior.ObservedFrameRate);
            }

            device.IsBrowserOverlayOnAir = _onAirBrowserOverlayIds.Contains(device.Id);
            device.IsBrowserOverlayInPreview = _previewBrowserOverlayIds.Contains(device.Id);

            CaptureDevices.Add(device);
        }

        NormalizeCaptureAudioAssignments();
        RefreshDualCaptureSourceOptions();
        RefreshCaptureFleetSummary();
        RefreshShowInputEditors();
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
        OnPropertyChanged(nameof(HasCaptureDevices));
        OnPropertyChanged(nameof(BrowserCaptureDevices));
        OnPropertyChanged(nameof(HasBrowserCaptureDevices));
        NotifyBrowserOverlayControlStateChanged();
        RebuildAudioCaptureDeviceCatalog();
        QueueSelectedCaptureDevicesOnline();
    }

    private void RefreshVirtualSrtIngestDevices()
    {
        foreach (var source in SrtIngestSources)
        {
            RefreshVirtualSrtIngestDevice(source);
        }

        var activeIds = SrtIngestSources
            .Select(source => source.DeviceId)
            .ToHashSet(StringComparer.Ordinal);
        foreach (var staleDevice in CaptureDevices
            .Where(device => IsVirtualSrtIngestDevice(device) && !activeIds.Contains(device.Id))
            .ToList())
        {
            RemoveVirtualSrtIngestDevice(staleDevice.Id);
        }
    }

    private void RefreshVirtualSrtIngestDevice(SrtIngestSource source)
    {
        var prior = CaptureDevices.FirstOrDefault(device => device.Id == source.DeviceId);
        if (prior is null)
        {
            CaptureDevices.Add(CreateVirtualSrtIngestDevice(source));
        }
        else
        {
            var index = CaptureDevices.IndexOf(prior);
            var next = CreateVirtualSrtIngestDevice(source);
            next.ConnectionState = prior.ConnectionState;
            next.SignalPresent = prior.SignalPresent;
            next.SelectedInputId = prior.SelectedInputId;
            next.AudioSyncOffsetMs = prior.AudioSyncOffsetMs;
            next.AssignedAudioDeviceId = prior.AssignedAudioDeviceId;
            next.AssignedAudioDeviceName = prior.AssignedAudioDeviceName;
            next.ObservedFrameWidth = prior.ObservedFrameWidth;
            next.ObservedFrameHeight = prior.ObservedFrameHeight;
            next.ObservedFrameRate = prior.ObservedFrameRate;
            CaptureDevices[index] = next;
        }

        NormalizeCaptureAudioAssignments();
        RefreshDualCaptureSourceOptions();
        RefreshCaptureFleetSummary();
        OnPropertyChanged(nameof(HasCaptureDevices));
    }

    private void RemoveVirtualSrtIngestDevice(string deviceId)
    {
        var device = CaptureDevices.FirstOrDefault(item => string.Equals(item.Id, deviceId, StringComparison.Ordinal));
        if (device is not null)
        {
            CaptureDevices.Remove(device);
        }

        foreach (var slot in ShowInputs.Where(slot =>
            slot.Kind == ShowInputKind.SrtIngest &&
            string.Equals(slot.CaptureDeviceId, deviceId, StringComparison.Ordinal)))
        {
            slot.Kind = ShowInputKind.Unassigned;
            slot.CaptureDeviceId = null;
            slot.InShow = false;
        }

        foreach (var route in _sceneRoutes.Values.SelectMany(routes => routes)
            .Where(route => route.Mode == SourceRouteMode.CaptureDevice &&
                string.Equals(route.CaptureDeviceId, deviceId, StringComparison.Ordinal)))
        {
            route.Mode = SourceRouteMode.None;
            route.CaptureDeviceId = null;
            route.ParticipantId = null;
        }

        RefreshDualCaptureSourceOptions();
        RefreshCaptureFleetSummary();
        RefreshShowInputEditors();
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
        OnPropertyChanged(nameof(HasCaptureDevices));
    }

    private IReadOnlyList<CaptureDevice> CreateVirtualSrtIngestDevices() =>
        SrtIngestSources.Select(CreateVirtualSrtIngestDevice).ToList();

    private static SrtIngestSource CreateSrtIngestSource(int number) =>
        new()
        {
            Id = $"srt-source-{number:00}",
            Number = number,
            Port = (10000 + number - 1).ToString()
        };

    private static CaptureDevice CreateVirtualSrtIngestDevice(SrtIngestSource source) =>
        new()
        {
            Id = source.DeviceId,
            NativeDeviceId = source.NativeUri,
            Vendor = "srt",
            Name = $"{source.Name} - {source.Summary}",
            Inputs =
            [
                new CaptureDeviceInput
                {
                    Id = source.Id,
                    Label = source.Name
                }
            ],
            SelectedInputId = source.Id,
            Width = 1920,
            Height = 1080,
            FrameRate = 60,
            ConnectionState = CaptureConnectionState.Detected,
            SignalPresent = false,
            AudioSyncOffsetMs = 0
        };

    private void RefreshProductionReadouts()
    {
        _automationRecommendation = ProductionStateHelper.BuildAutomationRecommendation(
            _bridge.LastSnapshot?.AutoProduction,
            RoomVideoParticipants,
            Scenes,
            AutomationPreferScreenShare,
            (int)Math.Round(AutomationPanelParticipantThreshold));
        FeedHealthRows = ProductionStateHelper.BuildFeedHealthRows(RoomVideoParticipants, _participantProductionRoles);
        FeedHealthSummary = ProductionStateHelper.FeedHealthSummary(RoomVideoParticipants);
        MagicSceneStatus = ProductionStateHelper.BuildMagicSceneStatus(RoomVideoParticipants);
        MediaBinSummary = ProductionStateHelper.MediaBinSummary(MediaBinGroups.Sum(group => group.Assets.Count));
        AutoProductionReadout = BuildAutoProductionReadout();
        RefreshAudioMixChannels();
        RefreshCaptureFleetSummary();

        OnPropertyChanged(nameof(HasCaptureDevices));
        OnPropertyChanged(nameof(HasMediaBinAssets));
        OnPropertyChanged(nameof(HasCaptionTranscript));
        OnPropertyChanged(nameof(FeedHealthRows));
        NotifyShowReadinessChanged();
        OnPropertyChanged(nameof(SceneIntelligenceSummary));
        OnPropertyChanged(nameof(RecommendedSceneName));
        OnPropertyChanged(nameof(RecommendedLayout));
        OnPropertyChanged(nameof(RecommendedConfidence));
        OnPropertyChanged(nameof(AutoProductionReason));
        OnPropertyChanged(nameof(AutoSwitchLabel));
        OnPropertyChanged(nameof(AutomationPolicySummary));
        OnPropertyChanged(nameof(AutomationScenePolicySummary));
        OnPropertyChanged(nameof(AutomationOverlayPolicySummary));
        OnPropertyChanged(nameof(AutomationTakeModeLabel));
        OnPropertyChanged(nameof(CaptionQualitySummary));
        RefreshAudioReadoutBindings();
        EvaluateAutomationPolicy();
    }

    private void RefreshAudioReadoutBindings()
    {
        OnPropertyChanged(nameof(AudioMix));
        OnPropertyChanged(nameof(LoudnessTargetLabel));
        OnPropertyChanged(nameof(LoudnessLevelLabel));
        OnPropertyChanged(nameof(TruePeakLabel));
        OnPropertyChanged(nameof(MasterLimiterModeLabel));
        OnPropertyChanged(nameof(MasterLimiterActivityLabel));
        OnPropertyChanged(nameof(MasterLimiterSummary));
        OnPropertyChanged(nameof(MasteringActivityLabel));
        RefreshAudioMonitorBindings();
    }

    private void RefreshAudioMonitorBindings()
    {
        OnPropertyChanged(nameof(SelectedAudioMonitorDeviceName));
        OnPropertyChanged(nameof(AudioMonitorVolumeLabel));
        OnPropertyChanged(nameof(AudioMonitorStatus));
        OnPropertyChanged(nameof(AudioMonitorEngineStatus));
        OnPropertyChanged(nameof(AudioProofSummary));
        OnPropertyChanged(nameof(AudioValidationSummary));
        OnPropertyChanged(nameof(AudioValidationChecklist));
        OnPropertyChanged(nameof(AudioValidationFirstIssue));
        OnPropertyChanged(nameof(AudioFullChainValidationSummary));
        OnPropertyChanged(nameof(AudioMeterSourceSummary));
        OnPropertyChanged(nameof(CaptureAudioSignalSummary));
        OnPropertyChanged(nameof(AudioBusTapSummary));
        OnPropertyChanged(nameof(CaptureAudioSourceSummary));
        OnPropertyChanged(nameof(StudioMonitorSummary));
        OnPropertyChanged(nameof(LocalAudioSourceStatus));
    }

    private void MaybeLogAudioTelemetry(NativeMediaCoreStateSnapshot snapshot)
    {
        var audio = snapshot.AudioMixSession;
        var capture = snapshot.CaptureAudioSources;
        var matrix = snapshot.AudioRoutingMatrix;
        var fullChainValidation = AudioValidationChecklistBuilder.SummarizeFullChain(
            audio,
            capture,
            snapshot.OutputSenderSession,
            snapshot.Recording);
        var now = DateTimeOffset.UtcNow;
        // STATE-shaped fields only. The signature previously embedded monotonic
        // counters (mixedFrames, monitorFrames, captureFrames, routed/tap frame
        // counts) and live per-tick dB strings, so it changed on EVERY snapshot
        // and the 5s throttle below never engaged — the line printed ~4x/second
        // for any live session. Counters still appear in the logged LINE; they
        // just no longer defeat the throttle.
        var signature =
            $"{LocalAudioSourceEnabled}|{SelectedLocalAudioCaptureDeviceId}|{SelectedAudioMonitorDeviceId}|{AudioMonitoringEnabled}|" +
            $"{audio.Status}|{audio.MonitorStatus}|" +
            $"{capture.Status}|{capture.SourceCount}|{capture.StreamingCount}|" +
            $"{matrix.Status}|{matrix.RoutedSendCount}|{matrix.BusTaps.Count}|" +
            $"{snapshot.OutputSenderSession.Status}|{snapshot.OutputSenderSession.ActiveSenderCount}|" +
            $"{fullChainValidation}|" +
            $"{FirstOrEmpty(audio.Warnings)}|{FirstOrEmpty(capture.Warnings)}|{FirstOrEmpty(matrix.Warnings)}";

        if (string.Equals(signature, _lastAudioTelemetrySignature, StringComparison.Ordinal) &&
            now - _lastAudioTelemetryLoggedAt < TimeSpan.FromSeconds(5))
        {
            return;
        }

        _lastAudioTelemetrySignature = signature;
        _lastAudioTelemetryLoggedAt = now;

        LaunchLog.Write(
            "audio: telemetry " +
            $"localEnabled={LocalAudioSourceEnabled} " +
            $"localDevice={TelemetryValue(SelectedLocalAudioCaptureDeviceName)} " +
            $"localStatus={TelemetryValue(LocalAudioSourceStatus)} " +
            $"monitorEnabled={AudioMonitoringEnabled} " +
            $"monitorDevice={TelemetryValue(SelectedAudioMonitorDeviceName)} " +
            $"mixStatus={TelemetryValue(audio.Status)} " +
            $"mixedFrames={audio.MixedFrameCount} " +
            $"master={audio.MasterLevel} " +
            $"monitorStatus={TelemetryValue(audio.MonitorStatus ?? "unknown")} " +
            $"monitorFrames={audio.MonitorFramesPlayed} " +
            $"captureStatus={TelemetryValue(capture.Status)} " +
            $"captureSources={capture.StreamingCount}/{capture.SourceCount} " +
            $"captureFrames={capture.CaptureFramesReceived} " +
            $"routedMaster={capture.RoutedMasterFrames} " +
            $"routedMon={capture.RoutedMonitorFrames} " +
            $"fallbackMon={capture.FallbackMonitorFrames} " +
            $"routeStatus={TelemetryValue(matrix.Status)} " +
            $"routedSends={matrix.RoutedSendCount} " +
            $"programTapFrames={matrix.ProgramTapFrames} " +
            $"fullChain={TelemetryValue(fullChainValidation, 192)} " +
            $"senders={TelemetryValue(BuildOutputSenderTelemetry(snapshot.OutputSenderSession), 512)} " +
            $"busTaps={TelemetryValue(BuildAudioBusTapTelemetry(matrix), 256)} " +
            $"sources={TelemetryValue(BuildCaptureAudioSourceTelemetry(capture), 512)} " +
            $"warnings={TelemetryValue(BuildAudioWarningTelemetry(audio, capture, matrix), 384)}");
    }

    private void OnAudioMonitorSettingsChanged()
    {
        if (AudioMonitoringEnabled)
        {
            var resolvedMonitorDeviceId = ResolveAudioMonitorDeviceId(SelectedAudioMonitorDeviceId, AudioRenderDevices);
            if (!string.Equals(resolvedMonitorDeviceId, SelectedAudioMonitorDeviceId, StringComparison.Ordinal))
            {
                SelectedAudioMonitorDeviceId = resolvedMonitorDeviceId;
                return;
            }
        }

        RefreshAudioMonitorBindings();
        SaveProductionOutputPreferences();
        _ = TrySyncMediaCoreAsync();
    }

    public static string ResolveLocalAudioSourceDeviceId(
        string? selectedDeviceId,
        IEnumerable<AudioCaptureDevice> devices)
    {
        var deviceList = devices.ToList();
        if (!string.IsNullOrWhiteSpace(selectedDeviceId) &&
            deviceList.Any(device =>
                string.Equals(device.Id, selectedDeviceId, StringComparison.Ordinal)))
        {
            return selectedDeviceId!;
        }

        return AudioCaptureDeviceDiscoveryService.ResolveDefaultLocalMachineAudioDeviceId(deviceList);
    }

    public static string ResolveAudioMonitorDeviceId(
        string? selectedDeviceId,
        IEnumerable<AudioRenderDevice> devices)
    {
        var deviceList = devices.ToList();
        if (!string.IsNullOrWhiteSpace(selectedDeviceId) &&
            deviceList.Any(device => string.Equals(device.Id, selectedDeviceId, StringComparison.Ordinal)))
        {
            return selectedDeviceId!;
        }

        return deviceList
            .OrderBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .Select(device => device.Id)
            .FirstOrDefault() ?? string.Empty;
    }

    private static string BuildAudioWarningTelemetry(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreAudioRoutingMatrix matrix)
    {
        var warnings = audio.Warnings
            .Concat(capture.Warnings)
            .Concat(matrix.Warnings)
            .Where(warning => !string.IsNullOrWhiteSpace(warning))
            .Distinct(StringComparer.Ordinal)
            .Take(3)
            .ToArray();
        return warnings.Length == 0 ? "none" : string.Join("|", warnings);
    }

    private static string BuildCaptureAudioSourceTelemetry(NativeMediaCoreCaptureAudioSources capture)
    {
        if (capture.Sources.Count == 0)
        {
            return "none";
        }

        return string.Join(
            ",",
            capture.Sources
                .OrderByDescending(source => source.Paired)
                .ThenByDescending(source => source.CaptureStreaming)
                .ThenBy(source => source.CaptureDeviceId, StringComparer.Ordinal)
                .ThenBy(source => source.SourceId, StringComparer.Ordinal)
                .Take(8)
                .Select(source =>
                    $"{TelemetryValue(source.CaptureDeviceId)}:" +
                    $"{TelemetryValue(source.SourceId ?? "unpaired")}:" +
                    $"{TelemetryValue(source.AudioSourceKind ?? "unknown")}:" +
                    $"paired={source.Paired}:" +
                    $"streaming={source.CaptureStreaming}:" +
                    $"frames={source.CaptureFramesReceived}:" +
                    $"rendered={source.CaptureFramesRendered}:" +
                    $"queued={source.CaptureQueuedFrames}:" +
                    $"underruns={source.CaptureUnderrunCount}:" +
                    $"ageMs={source.CaptureLastFrameAgeMs:0}:" +
                    $"emptyPolls={source.EmptyPacketPolls}:" +
                    $"signal={source.SignalPresent}:" +
                    $"peak={source.PeakDbfs:0.0}dB:" +
                    $"rms={source.RmsDbfs:0.0}dB:" +
                    $"fmt={source.CaptureSampleRate}x{source.CaptureChannels}:" +
                    $"endpoint={TelemetryValue(source.EndpointName ?? source.EndpointId ?? "unknown")}:" +
                    $"lastError={TelemetryValue(source.LastError ?? "none")}:" +
                    $"warning={TelemetryValue(source.Warning ?? "none")}"));
    }

    private static string BuildAudioBusTapTelemetry(NativeMediaCoreAudioRoutingMatrix matrix)
    {
        if (matrix.BusTaps.Count == 0)
        {
            return "none";
        }

        return string.Join(
            ",",
            matrix.BusTaps
                .OrderByDescending(tap => tap.Frames)
                .Take(6)
                .Select(tap =>
                    $"{TelemetryValue(tap.BusId)}:{tap.Frames}f:{tap.PeakDbfs:0.0}dB"));
    }

    private static string BuildOutputSenderTelemetry(NativeMediaCoreOutputSenderSession session)
    {
        if (session.Senders.Count == 0)
        {
            return $"{TelemetryValue(session.Status)}:{session.ActiveSenderCount}:none";
        }

        return string.Join(
            ",",
            session.Senders
                .OrderByDescending(sender => sender.FramesSent)
                .ThenBy(sender => sender.Destination, StringComparer.Ordinal)
                .Take(6)
                .Select(sender =>
                    $"{TelemetryValue(sender.Destination)}:" +
                    $"{TelemetryValue(sender.Status)}:" +
                    $"frames={sender.FramesSent}:" +
                    $"audio={sender.AudioFramesSent}f:" +
                    $"result={TelemetryValue(sender.LastResultCode ?? "none")}:" +
                    $"warning={TelemetryValue(sender.Warning ?? sender.LastError ?? "none", 128)}:" +
                    $"runtime={TelemetryValue(sender.RuntimeDetail ?? "none", 128)}"));
    }

    private static string FirstOrEmpty(IReadOnlyList<string> values) =>
        values.FirstOrDefault(value => !string.IsNullOrWhiteSpace(value)) ?? string.Empty;

    private static string TelemetryValue(string? value, int maxLength = 96)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return "none";
        }

        var cleaned = value
            .Replace("\r", " ", StringComparison.Ordinal)
            .Replace("\n", " ", StringComparison.Ordinal)
            .Replace(";", ",", StringComparison.Ordinal);
        if (maxLength <= 3)
        {
            return cleaned.Length <= maxLength ? cleaned : cleaned[..Math.Max(0, maxLength)];
        }

        return cleaned.Length <= maxLength ? cleaned : $"{cleaned[..(maxLength - 3)]}...";
    }

    public static string FormatRecordingFailureStatus(string action, Exception exception)
    {
        var detail = NormalizeStreamingFailureDetail(exception.Message);
        var lowered = detail.ToLowerInvariant();
        var mediaCoreFailure =
            lowered.Contains("media core", StringComparison.Ordinal) ||
            lowered.Contains("media-core", StringComparison.Ordinal) ||
            lowered.Contains("native media core", StringComparison.Ordinal) ||
            lowered.Contains("native-media-core", StringComparison.Ordinal) ||
            lowered.Contains("json-rpc", StringComparison.Ordinal) ||
            lowered.Contains("json rpc", StringComparison.Ordinal) ||
            lowered.Contains("broken pipe", StringComparison.Ordinal) ||
            lowered.Contains("process exited", StringComparison.Ordinal) ||
            lowered.Contains("process is not running", StringComparison.Ordinal);
        var prefix = lowered.Contains("still applying another output change", StringComparison.Ordinal) ||
                     lowered.Contains("try again", StringComparison.Ordinal) && lowered.Contains("media core", StringComparison.Ordinal)
            ? "Media core is busy applying changes. Wait a moment and try Record again."
            : lowered.Contains("program frame", StringComparison.Ordinal) ||
              lowered.Contains("program pixels", StringComparison.Ordinal)
                ? "Program video is not ready. Put a valid source on Program before recording."
            : lowered.Contains("target folder", StringComparison.Ordinal) ||
              lowered.Contains("path", StringComparison.Ordinal) ||
              lowered.Contains("directory", StringComparison.Ordinal) ||
              lowered.Contains("disk", StringComparison.Ordinal)
                ? "Recording target is not ready. Check the folder path and disk space."
            : mediaCoreFailure
                ? "Media core failed while starting recording. Open Details for the native error."
                : "Media core rejected the recording request.";

        var normalizedAction = string.IsNullOrWhiteSpace(action) ? "request" : action.Trim();
        return $"Recording {normalizedAction} failed: {prefix} {detail}";
    }

    public static string FormatStreamingFailureStatus(string action, Exception exception)
    {
        var rawLowered = exception.Message?.ToLowerInvariant() ?? string.Empty;
        var detail = NormalizeStreamingFailureDetail(exception.Message);
        var lowered = detail.ToLowerInvariant();
        var rtmpContext =
            rawLowered.Contains("rtmp", StringComparison.Ordinal) ||
            rawLowered.Contains("rtmps", StringComparison.Ordinal) ||
            lowered.Contains("rtmp", StringComparison.Ordinal) ||
            lowered.Contains("rtmps", StringComparison.Ordinal);
        var ffmpegRuntimeMissing =
            lowered.Contains("ffmpeg executable", StringComparison.Ordinal) ||
            lowered.Contains("ffmpeg.exe was not found", StringComparison.Ordinal) ||
            lowered.Contains("ffmpeg folder not found", StringComparison.Ordinal) ||
            lowered.Contains("does not contain ffmpeg.exe", StringComparison.Ordinal) ||
            lowered.Contains("choose the bin folder", StringComparison.Ordinal);
        var mediaCoreFailure =
            lowered.Contains("media core", StringComparison.Ordinal) ||
            lowered.Contains("media-core", StringComparison.Ordinal) ||
            lowered.Contains("native media core", StringComparison.Ordinal) ||
            lowered.Contains("native-media-core", StringComparison.Ordinal) ||
            lowered.Contains("json-rpc", StringComparison.Ordinal) ||
            lowered.Contains("json rpc", StringComparison.Ordinal) ||
            lowered.Contains("broken pipe", StringComparison.Ordinal) ||
            lowered.Contains("process exited", StringComparison.Ordinal) ||
            lowered.Contains("process is not running", StringComparison.Ordinal);
        var prefix = lowered.Contains("still applying another output change", StringComparison.Ordinal) ||
                     lowered.Contains("try again", StringComparison.Ordinal) && lowered.Contains("media core", StringComparison.Ordinal)
            ? "Media core is busy applying changes. Wait a moment and try Stream again."
            : lowered.Contains("program frame", StringComparison.Ordinal) ||
                     lowered.Contains("program pixels", StringComparison.Ordinal)
            ? "Program video is not ready. Put a valid source on Program before streaming."
            : lowered.Contains("did not arm a native output sender", StringComparison.Ordinal) ||
              lowered.Contains("sender state idle", StringComparison.Ordinal)
                ? "Native output sender did not start. Check Stream settings and open Health for sender diagnostics."
            : lowered.Contains("select at least one stream destination", StringComparison.Ordinal)
                ? "No stream destination is selected. Enable RTMP, NDI, or SRT before streaming."
            : lowered.Contains("configure rtmp", StringComparison.Ordinal) ||
              lowered.Contains("rtmp server url", StringComparison.Ordinal) ||
              lowered.Contains("rtmp stream key", StringComparison.Ordinal)
                ? "RTMP settings are incomplete. Configure the server URL and stream key before streaming."
            : lowered.Contains("configure an ndi program name", StringComparison.Ordinal)
                ? "NDI settings are incomplete. Set the NDI program name before streaming."
            : lowered.Contains("ndi", StringComparison.Ordinal) &&
              (lowered.Contains("output-unavailable", StringComparison.Ordinal) ||
               lowered.Contains("no ndi sender module", StringComparison.Ordinal) ||
               lowered.Contains("not available in this build", StringComparison.Ordinal) ||
               lowered.Contains("libndi runtime", StringComparison.Ordinal) ||
               lowered.Contains("runtime-missing", StringComparison.Ordinal) ||
               lowered.Contains("missing ndi-output", StringComparison.Ordinal))
                ? "NDI output is not available. Install the NDI runtime or use a build with NDI output enabled."
            : lowered.Contains("srt", StringComparison.Ordinal) &&
              (lowered.Contains("configure", StringComparison.Ordinal) ||
               lowered.Contains("must", StringComparison.Ordinal) ||
               lowered.Contains("passphrase", StringComparison.Ordinal))
                ? "SRT settings are incomplete. Check host, port, latency, and passphrase before streaming."
            : lowered.Contains("srt", StringComparison.Ordinal) &&
              (lowered.Contains("output-unavailable", StringComparison.Ordinal) ||
               lowered.Contains("no srt sender module", StringComparison.Ordinal) ||
               lowered.Contains("not available in this build", StringComparison.Ordinal) ||
               lowered.Contains("missing srt-output", StringComparison.Ordinal))
                ? "SRT output is not available in this build. Use RTMP/NDI or install a build with SRT output enabled."
            : lowered.Contains("ffmpeg", StringComparison.Ordinal) && ffmpegRuntimeMissing
                ? "FFmpeg is not ready. Choose the FFmpeg bin folder in Settings > FFmpeg."
                : rtmpContext ||
                  lowered.Contains("connection refused", StringComparison.Ordinal)
                    ? "RTMP output failed. Check the server URL, stream key, and network."
                    : mediaCoreFailure
                        ? "Media core failed while starting stream. Open Details for the native error."
                        : "Media core rejected the stream request.";

        var normalizedAction = string.IsNullOrWhiteSpace(action) ? "request" : action.Trim();
        return $"Streaming {normalizedAction} failed: {prefix} {detail}";
    }

    public static string FormatOutputStatusBrief(string? status)
    {
        if (string.IsNullOrWhiteSpace(status))
        {
            return "Outputs idle";
        }

        var normalized = status.Trim();
        if (normalized.StartsWith("Streaming start failed:", StringComparison.OrdinalIgnoreCase))
        {
            return FormatStreamingFailureBrief(normalized, "Streaming start failed");
        }

        if (normalized.StartsWith("Streaming stop failed:", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream stop failed";
        }

        if (normalized.StartsWith("Recording start failed:", StringComparison.OrdinalIgnoreCase))
        {
            return FormatRecordingFailureBrief(normalized, "Recording start failed");
        }

        if (normalized.StartsWith("Recording stop failed:", StringComparison.OrdinalIgnoreCase))
        {
            return "Recording stop failed";
        }

        if (normalized.StartsWith("Streaming settings sync failed:", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream settings failed";
        }

        if (normalized.StartsWith("Streaming settings failed:", StringComparison.OrdinalIgnoreCase))
        {
            return FormatStreamingFailureBrief(normalized, "Stream settings failed");
        }

        if (normalized.StartsWith("RTMP output failed:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("RTMPS output failed:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("SRT output failed:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("NDI output failed:", StringComparison.OrdinalIgnoreCase))
        {
            var separatorIndex = normalized.IndexOf(':', StringComparison.Ordinal);
            return separatorIndex > 0 ? normalized[..separatorIndex] : "Streaming failed";
        }

        if (normalized.StartsWith("Output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("Output failed:", StringComparison.OrdinalIgnoreCase))
        {
            if (normalized.Contains("rtmp", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("rtmps", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("ffmpeg", StringComparison.OrdinalIgnoreCase))
            {
                return normalized.Contains("failed", StringComparison.OrdinalIgnoreCase)
                    ? "RTMP output failed"
                    : "RTMP output warning";
            }

            return normalized.Contains("failed", StringComparison.OrdinalIgnoreCase)
                ? "Output failed"
                : "Output warning";
        }

        if (normalized.StartsWith("RTMP output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("RTMPS output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("SRT output warning:", StringComparison.OrdinalIgnoreCase) ||
            normalized.StartsWith("NDI output warning:", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream warning";
        }

        if (normalized.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
            normalized.Contains("error", StringComparison.OrdinalIgnoreCase))
        {
            return "Output failed";
        }

        return normalized.Length <= 28 ? normalized : $"{normalized[..25]}...";
    }

    private static string FormatRecordingFailureBrief(string normalized, string fallback)
    {
        if (normalized.Contains("Program video is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "Program video not ready";
        }

        if (normalized.Contains("Recording target is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "Recording target not ready";
        }

        if (normalized.Contains("Media core rejected", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core rejected recording";
        }

        if (normalized.Contains("Media core failed", StringComparison.OrdinalIgnoreCase))
        {
            if (normalized.Contains("process exited", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("process is not running", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core exited";
            }

            if (normalized.Contains("broken pipe", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core pipe failed";
            }

            return "Media core failed";
        }

        if (normalized.Contains("Media core is busy", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core busy";
        }

        return fallback;
    }

    private static string FormatStreamingFailureBrief(string normalized, string fallback)
    {
        if (normalized.Contains("Program video is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "Program video not ready";
        }

        if (normalized.Contains("Native output sender did not start", StringComparison.OrdinalIgnoreCase))
        {
            return "Stream sender not armed";
        }

        if (normalized.Contains("RTMP output failed", StringComparison.OrdinalIgnoreCase))
        {
            return "RTMP output failed";
        }

        if (normalized.Contains("No stream destination is selected", StringComparison.OrdinalIgnoreCase))
        {
            return "No stream destination";
        }

        if (normalized.Contains("RTMP settings are incomplete", StringComparison.OrdinalIgnoreCase))
        {
            return "RTMP settings missing";
        }

        if (normalized.Contains("NDI settings are incomplete", StringComparison.OrdinalIgnoreCase))
        {
            return "NDI settings missing";
        }

        if (normalized.Contains("NDI output is not available", StringComparison.OrdinalIgnoreCase))
        {
            return "NDI unavailable";
        }

        if (normalized.Contains("SRT settings are incomplete", StringComparison.OrdinalIgnoreCase))
        {
            return "SRT settings missing";
        }

        if (normalized.Contains("SRT output is not available", StringComparison.OrdinalIgnoreCase))
        {
            return "SRT unavailable";
        }

        if (normalized.Contains("FFmpeg is not ready", StringComparison.OrdinalIgnoreCase))
        {
            return "FFmpeg not ready";
        }

        if (normalized.Contains("Media core rejected", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core rejected stream";
        }

        if (normalized.Contains("Media core failed", StringComparison.OrdinalIgnoreCase))
        {
            if (normalized.Contains("process exited", StringComparison.OrdinalIgnoreCase) ||
                normalized.Contains("process is not running", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core exited";
            }

            if (normalized.Contains("broken pipe", StringComparison.OrdinalIgnoreCase))
            {
                return "Native core pipe failed";
            }

            return "Media core failed";
        }

        if (normalized.Contains("Media core is busy", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core busy";
        }

        return fallback;
    }

    public static bool ShouldShowOutputStatusDetails(string? status)
    {
        if (string.IsNullOrWhiteSpace(status))
        {
            return false;
        }

        var normalized = status.Trim();
        return normalized.Length > 28 ||
            normalized.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
            normalized.Contains("error", StringComparison.OrdinalIgnoreCase) ||
            normalized.Contains("rejected", StringComparison.OrdinalIgnoreCase);
    }

    private static string NormalizeStreamingFailureDetail(string? message)
    {
        var detail = string.IsNullOrWhiteSpace(message)
            ? "No native error detail was returned."
            : message.Trim();

        if (detail.Contains("media-core sync in flight", StringComparison.OrdinalIgnoreCase) ||
            detail.Contains("sync in flight", StringComparison.OrdinalIgnoreCase) &&
            detail.Contains("backpressure", StringComparison.OrdinalIgnoreCase))
        {
            return "Media core is still applying another output change. Wait a moment and try again.";
        }

        string[] noisyPrefixes =
        [
            "media-core sync failed:",
            "native-media-core-sync failed:",
            "native media core sync failed:",
            "media-core request failed:",
            "native media core request failed:",
            "start-program-output failed:",
            "start-program-output failed.",
            "program-output failed:",
            "program-output failed.",
            "stop-program-output failed:",
            "stop-program-output failed.",
            "output sender failed during sync:",
            "output sender failed:",
            "rtmp output sender failed:",
            "rtmp sender failed:"
        ];

        var changed = true;
        while (changed)
        {
            changed = false;
            foreach (var prefix in noisyPrefixes)
            {
                if (!detail.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                detail = detail[prefix.Length..].Trim();
                changed = true;
            }
        }

        if (detail.Equals("missing:ffmpeg executable", StringComparison.OrdinalIgnoreCase))
        {
            return "FFmpeg executable was not found in the configured bin folder, app folder, or PATH.";
        }

        return detail;
    }

    public static string FormatAudioMonitorEngineStatus(NativeMediaCoreAudioMixSession audio)
        => FormatAudioMonitorEngineStatus(audio, capture: null);

    public static string FormatAudioMonitorEngineStatus(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources? capture)
    {
        if (audio.MixedFrameCount <= 0)
        {
            return "No mixed audio frames from the media engine.";
        }

        var monitorState = audio.MonitorEnabled
            ? FormatMonitorStatus(audio)
            : "muted";
        var status = $"{audio.MixedFrameCount} mixed frames - monitor {monitorState}";
        if (capture is null)
        {
            return status;
        }

        if (!audio.MonitorEnabled && capture.RoutedMonitorFrames > 0)
        {
            return $"{status} - MON bus has {capture.RoutedMonitorFrames} frames ready";
        }

        if (audio.MonitorEnabled &&
            audio.MonitorFramesPlayed > 0 &&
            capture.RoutedMonitorFrames <= 0 &&
            (capture.FallbackMonitorFrames > 0 || capture.CaptureFramesReceived > 0 || capture.RoutedMasterFrames > 0))
        {
            return $"{status} - fallback monitor mix {Math.Max(capture.FallbackMonitorFrames, (int)audio.MonitorFramesPlayed)} frames; no routed MON bus";
        }

        if (audio.MonitorEnabled && capture.RoutedMonitorFrames <= 0)
        {
            return $"{status} - {FormatMonitorBusEvidence(capture)}";
        }

        if (audio.MonitorEnabled && audio.MonitorFramesPlayed <= 0 && capture.RoutedMonitorFrames > 0)
        {
            return $"{status} - MON bus {capture.RoutedMonitorFrames} frames, no hardware playback frames";
        }

        if (audio.MonitorEnabled && capture.RoutedMonitorFrames > 0)
        {
            return $"{status} - MON bus {capture.RoutedMonitorFrames} frames";
        }

        return status;
    }

    public static string FormatStudioMonitorSummary(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        bool monitorToggleEnabled,
        string selectedDeviceName)
    {
        var monBus = FormatMonitorBusEvidence(capture);
        if (!monitorToggleEnabled || !audio.MonitorEnabled)
        {
            return $"Monitor off - {monBus}";
        }

        var device = string.IsNullOrWhiteSpace(selectedDeviceName)
            ? "No monitor output selected"
            : selectedDeviceName;
        if (audio.MonitorStatus == "missing-device")
        {
            return $"Monitor needs output device - {monBus}";
        }

        if (audio.MonitorFramesPlayed <= 0 && capture.RoutedMonitorFrames > 0)
        {
            return $"Monitor {FormatMonitorStatus(audio)} - {device} - {monBus}, no hardware playback frames";
        }

        if (audio.MonitorFramesPlayed > 0 &&
            capture.RoutedMonitorFrames <= 0 &&
            (capture.FallbackMonitorFrames > 0 || capture.CaptureFramesReceived > 0 || capture.RoutedMasterFrames > 0))
        {
            return $"Monitor {FormatMonitorStatus(audio)} - {device} - fallback monitor mix {Math.Max(capture.FallbackMonitorFrames, (int)audio.MonitorFramesPlayed)} frames; no routed MON bus";
        }

        return $"Monitor {FormatMonitorStatus(audio)} - {device} - {monBus}";
    }

    private static string FormatMonitorBusEvidence(NativeMediaCoreCaptureAudioSources capture)
    {
        if (capture.RoutedMonitorFrames > 0)
        {
            return $"MON bus {capture.RoutedMonitorFrames} frames";
        }

        if (capture.CaptureFramesReceived > 0 || capture.RoutedMasterFrames > 0)
        {
            if (capture.FallbackMonitorFrames > 0)
            {
                return $"fallback monitor mix {capture.FallbackMonitorFrames} frames; no routed MON bus";
            }

            return $"no PCM on MON bus; source PCM {capture.CaptureFramesReceived}, PGM {capture.RoutedMasterFrames}";
        }

        return "no PCM on MON bus";
    }

    public static string FormatMonitorStatus(NativeMediaCoreAudioMixSession audio) =>
        audio.MonitorStatus switch
        {
            "playing" => $"{audio.MonitorFramesPlayed} playback frames",
            "stub-monitor" => "stub only - no hardware output",
            "armed" => "armed, waiting for audio",
            "volume-zero" => "volume at 0%",
            "dropping" => "dropping monitor samples",
            "missing-device" => "needs output device",
            "muted" => "muted",
            "unavailable" => "unavailable",
            _ => audio.MonitorStatus ?? "unknown"
        };

    public static string FormatAudioProofSummary(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null)
    {
        // State words, not counters: this string sits in the transport bar, and
        // an ever-incrementing frame count reads as debug noise to an operator
        // (spec 4.4 — "replace the AUDIO PROOF counter string"). The raw
        // counters still go to launch.log via MaybeLogAudioTelemetry.
        var sourceState = capture.SourceCount <= 0
            ? "sources none"
            : $"sources {capture.StreamingCount}/{capture.SourceCount}";
        var pcmState = capture.CaptureFramesReceived > 0
            ? "PCM live"
            : "PCM none";
        var mixState = audio.MixedFrameCount > 0
            ? "mix live"
            : "mix none";
        var pgmState = capture.RoutedMasterFrames > 0
            ? "PGM live"
            : "PGM none";
        var monState = capture.RoutedMonitorFrames > 0
            ? "MON live"
            : capture.FallbackMonitorFrames > 0
                ? "MON fallback"
            : "MON none";
        var playbackState = audio.MonitorEnabled
            ? audio.MonitorFramesPlayed > 0
                ? "playback live"
                : $"playback none ({FormatMonitorStatus(audio)})"
            : "monitor off";
        var sourceDetail = capture.Sources.FirstOrDefault(source => source.CaptureStreaming || source.CaptureFramesReceived > 0) ??
            capture.Sources.FirstOrDefault();
        var sourceLabel = sourceDetail is null
            ? string.Empty
            : $" - {FormatAudioProofSourceLabel(sourceDetail)}";
        var outputState = FormatAudioOutputProofState(outputSenderSession);
        var recordingState = FormatRecordingAudioProofState(recording);
        var faultLabel = FormatAudioProofFaultLabel(audio, capture, outputSenderSession, recording);

        return $"{sourceState} | {pcmState} | {mixState} | {pgmState} | {monState} | {playbackState}{outputState}{recordingState}{faultLabel}{sourceLabel}";
    }

    private static string FormatAudioProofFaultLabel(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources capture,
        NativeMediaCoreOutputSenderSession? outputSenderSession = null,
        NativeMediaCoreRecordingSession? recording = null)
    {
        var faults = new List<string>();
        if (capture.SourceCount > 0 && capture.CaptureFramesReceived <= 0)
        {
            faults.Add("check source PCM");
        }

        if (capture.CaptureFramesReceived > 0 && capture.RoutedMasterFrames <= 0)
        {
            faults.Add("route source to PGM");
        }

        if (capture.RoutedMasterFrames > 0 &&
            capture.RoutedMonitorFrames <= 0 &&
            capture.FallbackMonitorFrames <= 0 &&
            (!audio.MonitorEnabled || audio.MonitorFramesPlayed <= 0))
        {
            faults.Add("route source to MON");
        }

        if (audio.MonitorEnabled && capture.RoutedMonitorFrames > 0 && audio.MonitorFramesPlayed <= 0)
        {
            faults.Add("check monitor output");
        }

        if (capture.RoutedMasterFrames > 0 &&
            outputSenderSession is { ActiveSenderCount: > 0 } &&
            outputSenderSession.Senders.Any(sender => string.Equals(sender.Status, "live", StringComparison.OrdinalIgnoreCase)) &&
            outputSenderSession.Senders.Where(sender => string.Equals(sender.Status, "live", StringComparison.OrdinalIgnoreCase)).Sum(sender => sender.AudioFramesSent) <= 0)
        {
            faults.Add("check stream audio");
        }

        if (capture.RoutedMasterFrames > 0 &&
            recording is { Active: true } &&
            recording.Proof is null)
        {
            faults.Add("check recording proof");
        }
        else if (capture.RoutedMasterFrames > 0 &&
                 recording is { Active: true } &&
                 (recording.Proof?.AudioSampleCount ?? 0) <= 0 &&
                 (recording.Proof?.AudioPacketsObserved ?? 0) <= 0)
        {
            faults.Add("check recording audio");
        }

        return faults.Count > 0
            ? $" | {string.Join("; ", faults)}"
            : string.Empty;
    }

    private static string FormatAudioOutputProofState(NativeMediaCoreOutputSenderSession? outputSenderSession)
    {
        if (outputSenderSession is not { ActiveSenderCount: > 0 })
        {
            return string.Empty;
        }

        var liveSenders = outputSenderSession.Senders
            .Where(sender => string.Equals(sender.Status, "live", StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (liveSenders.Length == 0)
        {
            return " | stream audio waiting";
        }

        var audioFrames = liveSenders.Sum(sender => sender.AudioFramesSent);
        if (audioFrames > 0)
        {
            var sampleRate = liveSenders.Select(sender => sender.AudioSampleRate).FirstOrDefault(value => value > 0);
            return sampleRate > 0
                ? $" | stream audio {audioFrames} frames @ {sampleRate} Hz"
                : $" | stream audio {audioFrames} frames";
        }

        return " | stream audio none";
    }

    private static string FormatRecordingAudioProofState(NativeMediaCoreRecordingSession? recording)
    {
        if (recording is not { Active: true })
        {
            return string.Empty;
        }

        if (recording.Proof is null)
        {
            return " | record proof missing";
        }

        if (recording.Proof is { AudioSampleCount: > 0 } proof)
        {
            return proof.AudioSampleRate > 0
                ? $" | record audio {proof.AudioSampleCount} samples @ {proof.AudioSampleRate} Hz"
                : $" | record audio {proof.AudioSampleCount} samples";
        }

        if (recording.Proof is { AudioPacketsObserved: > 0 } packetProof)
        {
            return $" | record audio {packetProof.AudioPacketsObserved} packets";
        }

        return " | record audio none";
    }

    private static string FormatAudioProofSourceLabel(NativeMediaCoreCaptureAudioSource source)
    {
        var label = !string.IsNullOrWhiteSpace(source.AudioDeviceName)
            ? source.AudioDeviceName
            : !string.IsNullOrWhiteSpace(source.AudioDeviceId)
                ? source.AudioDeviceId
                : source.CaptureDeviceId;
        var kind = string.IsNullOrWhiteSpace(source.AudioSourceKind)
            ? "audio"
            : source.AudioSourceKind;
        var state = source.CaptureStreaming
            ? source.CaptureFramesReceived > 0 ? "live" : "streaming, no frames"
            : source.Paired ? "paired idle" : "not paired";
        return $"{label} ({kind}, {state})";
    }

    public static string BuildAudioMeterSourceSummary(
        NativeMediaCoreAudioMixSession audio,
        NativeMediaCoreCaptureAudioSources? capture = null)
    {
        if (audio.MixedFrameCount <= 0)
        {
            return audio.Participants.Count > 0
                ? $"Meters idle: {audio.Participants.Count} channel(s) configured, no PCM mixed yet."
                : "Meters idle: no PCM channels reported by media core.";
        }

        var activeSources = audio.Participants
            .Where(participant => participant.OutputLevel > 0 || participant.InputLevel > 0 || participant.PeakDbfs > -59)
            .OrderByDescending(participant => participant.OutputLevel)
            .ThenByDescending(participant => participant.InputLevel)
            .Take(4)
            .Select(participant => FormatAudioMeterParticipantLabel(participant, capture))
            .ToList();
        var sourceLabel = activeSources.Count > 0
            ? string.Join(", ", activeSources)
            : audio.Participants.Count == 1
                ? FormatAudioMeterParticipantLabel(audio.Participants[0], capture)
                : $"{audio.Participants.Count} configured PCM channels, all silent";
        return audio.Warnings.Count > 0
            ? $"Meters: {sourceLabel}, warning: {audio.Warnings[0]}"
            : $"Meters: {sourceLabel}; monitor listens to MON bus.";
    }

    private static string FormatAudioMeterParticipantLabel(
        NativeMediaCoreParticipantAudioChannel participant,
        NativeMediaCoreCaptureAudioSources? capture)
    {
        var source = capture?.Sources.FirstOrDefault(item =>
            string.Equals(item.SourceId, participant.ParticipantId, StringComparison.Ordinal) ||
            string.Equals(ResolveCaptureTelemetrySourceId(item), participant.ParticipantId, StringComparison.Ordinal));
        var name = source is null
            ? participant.ParticipantId
            : !string.IsNullOrWhiteSpace(source.AudioDeviceName)
                ? source.AudioDeviceName
                : !string.IsNullOrWhiteSpace(source.EndpointName)
                    ? source.EndpointName
                    : source.CaptureDeviceId;
        var sourceKind = source is null
            ? "mix"
            : FormatAudioSourceKindForMeter(source.AudioSourceKind);
        var peak = double.IsFinite(participant.PeakDbfs) ? participant.PeakDbfs : -60;
        return $"{name} [{sourceKind}] {participant.OutputLevel}% peak {peak:0.0} dBFS";
    }

    private static string FormatAudioSourceKindForMeter(string? sourceKind) =>
        sourceKind?.Trim().ToLowerInvariant() switch
        {
            "wasapi-loopback" or "loopback" or "wasapi-render-loopback" or "system-loopback" => "loopback",
            "wasapi-input" or "wasapi-capture" => "input",
            "embedded-capture-audio" => "embedded",
            "asio-input" => "ASIO",
            "local-machine-audio" => "local",
            null or "" => "source",
            _ => sourceKind.Trim()
        };

    private static string ResolveCaptureTelemetrySourceId(NativeMediaCoreCaptureAudioSource source) =>
        string.Equals(source.CaptureDeviceId, "local-machine-audio", StringComparison.Ordinal)
            ? "local-machine-audio"
            : $"capture:{source.CaptureDeviceId}";

    private static string BuildCaptureAudioSignalSummary(NativeMediaCoreCaptureAudioSources capture)
    {
        var pcmState = capture.CaptureFramesReceived > 0
            ? $"{capture.CaptureFramesReceived} PCM frames received"
            : "no PCM frames received";
        var monitorState = capture.RoutedMonitorFrames > 0
            ? $"MON {capture.RoutedMonitorFrames}"
            : capture.FallbackMonitorFrames > 0
                ? $"MON fallback {capture.FallbackMonitorFrames}"
                : "MON 0";
        var signalCount = capture.Sources.Count(source => source.SignalPresent);
        var loudestPeak = capture.Sources.Count > 0
            ? capture.Sources.Max(source => source.PeakDbfs)
            : -120;
        var signalState = capture.CaptureFramesReceived > 0
            ? signalCount > 0
                ? $"{signalCount} source(s) with signal, peak {loudestPeak:0.0} dBFS"
                : "silent PCM"
            : "signal waiting";
        return $"{capture.StreamingCount}/{capture.SourceCount} source(s) streaming - {pcmState} - {signalState} - master {capture.RoutedMasterFrames}, stream {capture.RoutedStreamFrames}, {monitorState}, monitor {capture.MonitorFramesPlayed}";
    }

    private static string BuildAudioBusTapSummary(NativeMediaCoreAudioRoutingMatrix matrix)
    {
        if (matrix.BusTaps.Count == 0)
        {
            return matrix.RoutedSendCount > 0
                ? $"No measured PCM on routed buses yet; {matrix.RoutedSendCount} send(s) from {matrix.RoutedSourceCount} source(s)."
                : matrix.Summary;
        }

        var taps = matrix.BusTaps
            .OrderByDescending(tap => tap.Frames)
            .Take(4)
            .Select(tap => $"{FormatAudioBusLabel(tap.BusId)} {tap.Frames}f peak {tap.PeakDbfs:0.0} dBFS");
        return $"PCM bus taps: {string.Join(", ", taps)}";
    }

    private static string FormatAudioBusLabel(string busId) =>
        busId switch
        {
            "master" => "MASTER",
            "mon" => "MON",
            "stream" => "STREAM",
            "pgm-l" => "PGM L",
            "pgm-r" => "PGM R",
            _ when busId.StartsWith("aux-", StringComparison.OrdinalIgnoreCase) =>
                $"AUX {busId["aux-".Length..]}",
            _ => busId.ToUpperInvariant()
        };

    private string ResolveLocalAudioSourceStatus()
    {
        if (AudioCaptureDevices.All(device =>
                !string.Equals(device.Id, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal)))
        {
            return "Local source enabled - choose an audio input or loopback device";
        }

        var capture = _bridge.LastSnapshot?.CaptureAudioSources;
        return FormatLocalAudioSourceStatus(
            SelectedLocalAudioCaptureDeviceId,
            SelectedLocalAudioCaptureDeviceName,
            capture);
    }

    private string ResolveLocalAudioOperatorStatus()
    {
        if (AudioCaptureDevices.All(device =>
                !string.Equals(device.Id, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal)))
        {
            return "Choose an audio input or loopback device";
        }

        var source = _bridge.LastSnapshot?.CaptureAudioSources?.Sources.FirstOrDefault(source =>
            string.Equals(source.CaptureDeviceId, "local-machine-audio", StringComparison.Ordinal) ||
            string.Equals(source.AudioDeviceId, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal) ||
            string.Equals(source.AudioDeviceName, SelectedLocalAudioCaptureDeviceName, StringComparison.Ordinal));
        if (source is null)
        {
            return $"Waiting for {SelectedLocalAudioCaptureDeviceName}";
        }

        var sourceLabel = !string.IsNullOrWhiteSpace(source.EndpointName)
            ? source.EndpointName
            : SelectedLocalAudioCaptureDeviceName;
        if (source.SignalPresent)
        {
            return $"Signal detected - {sourceLabel}";
        }

        if (!source.CaptureStreaming)
        {
            return $"Starting - {sourceLabel}";
        }

        return $"Connected - no signal from {sourceLabel}";
    }

    public static string FormatLocalAudioSourceStatus(
        string? selectedDeviceId,
        string selectedDeviceName,
        NativeMediaCoreCaptureAudioSources? capture)
    {
        var source = capture?.Sources.FirstOrDefault(source =>
            string.Equals(source.CaptureDeviceId, "local-machine-audio", StringComparison.Ordinal) ||
            string.Equals(source.AudioDeviceId, selectedDeviceId, StringComparison.Ordinal) ||
            string.Equals(source.AudioDeviceName, selectedDeviceName, StringComparison.Ordinal));
        return source is null
            ? $"Local source waiting for native PCM evidence - {selectedDeviceName}"
            : $"Local source {FormatCaptureAudioSourceStatus(source)}";
    }

    public static string FormatLocalAudioSourceRecommendation(
        string? selectedDeviceId,
        string selectedDeviceName,
        IEnumerable<AudioCaptureDevice> devices,
        NativeMediaCoreCaptureAudioSources? capture)
    {
        var source = capture?.Sources.FirstOrDefault(source =>
            string.Equals(source.CaptureDeviceId, "local-machine-audio", StringComparison.Ordinal) ||
            string.Equals(source.AudioDeviceId, selectedDeviceId, StringComparison.Ordinal) ||
            string.Equals(source.AudioDeviceName, selectedDeviceName, StringComparison.Ordinal));
        var alternate = devices
            .Where(device => device.IsAvailable)
            .Where(device => !string.Equals(device.Id, selectedDeviceId, StringComparison.Ordinal))
            .OrderBy(device => AudioCaptureDeviceDiscoveryService.SourceKindPriority(device.SourceKind))
            .ThenBy(device => device.IsDefault ? 0 : 1)
            .ThenBy(device => device.DriverName, StringComparer.OrdinalIgnoreCase)
            .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault();
        var alternateText = alternate is null
            ? string.Empty
            : $" Try {alternate.DisplayLabel}.";

        if (source is null)
        {
            return $"Waiting for audio from {selectedDeviceName}.{alternateText}";
        }

        var sourceLabel = !string.IsNullOrWhiteSpace(source.EndpointName)
            ? source.EndpointName
            : selectedDeviceName;

        if (source.SignalPresent)
        {
            return string.Empty;
        }

        if (source.CaptureStreaming && source.CaptureFramesReceived <= 0 && IsLoopbackAudioSourceKind(source.AudioSourceKind))
        {
            return $"No audio detected. Play audio through {sourceLabel}, or choose another source.{alternateText}";
        }

        if (source.CaptureFramesReceived > 0 && !source.SignalPresent)
        {
            return $"No audio signal detected from {sourceLabel}. Check that the device is active, or choose another source.{alternateText}";
        }

        if (!source.CaptureStreaming)
        {
            return $"{selectedDeviceName} is not active yet. Reselect it, or choose another source.{alternateText}";
        }

        return $"Waiting for audio from {sourceLabel}.{alternateText}";
    }

    public static string FormatCaptureAudioSourceStatus(NativeMediaCoreCaptureAudioSource source)
    {
        var label = !string.IsNullOrWhiteSpace(source.AudioDeviceName)
            ? source.AudioDeviceName
            : !string.IsNullOrWhiteSpace(source.AudioDeviceId)
                ? source.AudioDeviceId
                : source.CaptureDeviceId;
        var loopbackIdle = source.CaptureStreaming &&
            source.CaptureFramesReceived <= 0 &&
            IsLoopbackAudioSourceKind(source.AudioSourceKind);
        var state = loopbackIdle
            ? "loopback idle - play audio through this output or choose an input"
            : source.CaptureStreaming ? "streaming" : source.Paired ? "paired idle" : "not paired";
        var format = source.CaptureSampleRate > 0 && source.CaptureChannels > 0
            ? $" {source.CaptureSampleRate} Hz/{source.CaptureChannels} ch"
            : string.Empty;
        var endpoint = !string.IsNullOrWhiteSpace(source.EndpointName)
            ? $" via {source.EndpointName}"
            : string.Empty;
        var emptyPolls = source.EmptyPacketPolls > 0 && source.CaptureFramesReceived <= 0
            ? $", {source.EmptyPacketPolls} silent polls"
            : string.Empty;
        var level = source.CaptureFramesReceived > 0
            ? $", peak {source.PeakDbfs:0.0} dBFS, rms {source.RmsDbfs:0.0} dBFS" +
              (source.SignalPresent ? string.Empty : ", silent PCM")
            : string.Empty;
        var renderEvidence = source.CaptureFramesRendered > 0 && source.CaptureFramesRendered != source.CaptureFramesReceived
            ? $", rendered {source.CaptureFramesRendered}"
            : string.Empty;
        var queueEvidence = source.CaptureQueuedFrames > 0
            ? $", queued {source.CaptureQueuedFrames}"
            : string.Empty;
        var underrunEvidence = source.CaptureUnderrunCount > 0
            ? $", underruns {source.CaptureUnderrunCount}"
            : string.Empty;
        var timingEvidence = FormatCaptureAudioTimingEvidence(source);
        var lastError = !string.IsNullOrWhiteSpace(source.LastError)
            ? $", {source.LastError}"
            : string.Empty;
        var sourceId = !string.IsNullOrWhiteSpace(source.SourceId)
            ? $" -> {source.SourceId}"
            : string.Empty;
        var kind = !string.IsNullOrWhiteSpace(source.AudioSourceKind)
            ? $" ({source.AudioSourceKind})"
            : string.Empty;
        var warning = FormatCaptureAudioSourceWarningForOperator(source.Warning);
        var warningText = !string.IsNullOrWhiteSpace(warning)
            ? $", issue: {warning}"
            : string.Empty;
        return $"{label}{kind}{sourceId}: {state}, {source.CaptureFramesReceived} frames{renderEvidence}{queueEvidence}{underrunEvidence}{timingEvidence}{emptyPolls}{level}{format}{endpoint}{lastError}{warningText}";
    }

    private static string FormatCaptureAudioTimingEvidence(NativeMediaCoreCaptureAudioSource source)
    {
        var parts = new List<string>();
        if (source.CaptureStartedAtMs > 0)
        {
            parts.Add($"started {source.CaptureStartedAtMs:0}ms");
        }

        if (source.CaptureLastFrameAtMs > 0)
        {
            parts.Add($"last PCM {source.CaptureLastFrameAtMs:0}ms");
        }

        if (source.CaptureLastFrameAgeMs > 0)
        {
            parts.Add($"age {source.CaptureLastFrameAgeMs:0}ms");
        }

        if (source.CaptureStoppedAtMs > 0)
        {
            parts.Add($"stopped {source.CaptureStoppedAtMs:0}ms");
        }

        return parts.Count > 0
            ? $", {string.Join(", ", parts)}"
            : string.Empty;
    }

    public static string FormatCaptureAudioSourceWarningForOperator(string? warning)
    {
        if (string.IsNullOrWhiteSpace(warning))
        {
            return string.Empty;
        }

        if (warning.Contains("endpoint has not produced loopback packets", StringComparison.OrdinalIgnoreCase))
        {
            return "no loopback packets from selected output; play audio through that output or choose another source";
        }

        if (warning.Contains("receiving silent PCM frames", StringComparison.OrdinalIgnoreCase))
        {
            return "silent PCM from selected source; choose the active output/input or play audio through it";
        }

        if (warning.Contains("could not query", StringComparison.OrdinalIgnoreCase) ||
            warning.Contains("could not read", StringComparison.OrdinalIgnoreCase))
        {
            return "WASAPI read failed; reselect the device or restart the audio engine";
        }

        return warning.Length <= 140 ? warning : $"{warning[..137]}...";
    }

    public static string FormatNativeLowerThirdStatus(NativeMediaCoreOverlayState overlay)
    {
        if (overlay.OverlayCount <= 0 || overlay.LowerThirdCount <= 0)
        {
            return "Native: no lower third keyed.";
        }

        var lowerThird = overlay.Overlays.FirstOrDefault(item =>
            item.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase)) ??
            overlay.Overlays.FirstOrDefault();
        if (lowerThird is null)
        {
            return overlay.Summary;
        }

        var source = !string.IsNullOrWhiteSpace(lowerThird.SourceName)
            ? lowerThird.SourceName
            : !string.IsNullOrWhiteSpace(lowerThird.SourceId)
                ? lowerThird.SourceId
                : "unknown source";
        var phase = FormatLowerThirdPhaseLabel(lowerThird.KeyPhase);
        var title = !string.IsNullOrWhiteSpace(lowerThird.Title)
            ? $" - {lowerThird.Title}"
            : string.Empty;
        return $"Native: {phase} for {source}{title}; build {lowerThird.BuildInMs} ms / out {lowerThird.BuildOutMs} ms.";
    }

    public static string FormatNativeMediaPlaybackStatus(NativeMediaCoreMediaPlaybackState playback)
    {
        if (string.Equals(playback.Status, "idle", StringComparison.OrdinalIgnoreCase) ||
            string.IsNullOrWhiteSpace(playback.MediaAssetId))
        {
            return "Native: no media asset routed to Program.";
        }

        var name = !string.IsNullOrWhiteSpace(playback.MediaAssetName)
            ? playback.MediaAssetName
            : playback.MediaAssetId;
        var state = playback.Playing ? "playing" : "paused";
        var key = string.IsNullOrWhiteSpace(playback.MediaPlaybackKey)
            ? "no playback key"
            : playback.MediaPlaybackKey;
        return $"Native: {name} {state}; key {key}.";
    }

    public static string FormatNativeCoreRuntimeStatus(string? executablePath, NativeMediaCoreProfile? profile)
    {
        var path = string.IsNullOrWhiteSpace(executablePath)
            ? "not resolved"
            : Path.GetFullPath(executablePath);
        if (profile is null)
        {
            return $"Native core: {path}; profile waiting for handshake.";
        }

        var localAudio = profile.Capabilities.Any(capability =>
            string.Equals(capability, "local-audio-capture", StringComparison.OrdinalIgnoreCase))
                ? "local audio on"
                : "local audio missing";
        var monitor = profile.Capabilities.Any(capability =>
            string.Equals(capability, "audio-monitor-output", StringComparison.OrdinalIgnoreCase))
                ? "monitor output on"
                : "monitor output missing";

        return $"Native core: {path}; {profile.Name}; {profile.Renderer}; {localAudio}; {monitor}.";
    }

    public static string FormatNativeAudioRuntimeStatus(NativeMediaCoreProfile? profile)
    {
        if (profile is null)
        {
            return "Audio runtime: waiting for native profile handshake.";
        }

        string[] requiredAudioCapabilities = ["audio-mixer", "local-audio-capture", "audio-monitor-output"];
        var missing = requiredAudioCapabilities
            .Where(capability => !profile.Capabilities.Contains(capability, StringComparer.Ordinal))
            .ToList();

        if (missing.Count == 0)
        {
            return "Audio runtime: ready - native mixer, WASAPI capture, and monitor output are enabled.";
        }

        return $"Audio runtime: blocked - native profile missing {string.Join(", ", missing)}.";
    }

    private static bool IsLoopbackAudioSourceKind(string? sourceKind) =>
        sourceKind?.Trim().ToLowerInvariant() is
            "loopback" or
            "wasapi-loopback" or
            "wasapi-render-loopback" or
            "system-loopback";

    private void RefreshLocalAudioSourceBindings()
    {
        OnPropertyChanged(nameof(SelectedLocalAudioCaptureDeviceName));
        OnPropertyChanged(nameof(LocalAudioSourceStatus));
        OnPropertyChanged(nameof(LocalAudioOperatorStatus));
        OnPropertyChanged(nameof(LocalAudioSourceRecommendation));
        OnPropertyChanged(nameof(CaptureAudioSignalSummary));
        OnPropertyChanged(nameof(CaptureAudioSourceSummary));
    }

    private void RefreshAudioMixChannels()
    {
        var existing = _audioMixChannels
            .Where(channel => !string.IsNullOrWhiteSpace(channel.ParticipantId))
            .GroupBy(channel => channel.ParticipantId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.Last(), StringComparer.Ordinal);
        var merged = ProductionStateHelper.BuildAudioMixChannels(RoomVideoParticipants, existing).ToList();
        var mergedById = merged
            .Where(channel => !string.IsNullOrWhiteSpace(channel.ParticipantId))
            .GroupBy(channel => channel.ParticipantId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.Last(), StringComparer.Ordinal);

        if (_bridge.LastSnapshot?.AudioMixSession is { } nativeAudio)
        {
            foreach (var nativeChannel in nativeAudio.Participants)
            {
                if (string.IsNullOrWhiteSpace(nativeChannel.ParticipantId))
                {
                    continue;
                }

                existing.TryGetValue(nativeChannel.ParticipantId, out var prior);
                var channel = new ParticipantAudioMix
                {
                    ParticipantId = nativeChannel.ParticipantId,
                    OutputLevel = Math.Clamp(nativeChannel.OutputLevel, 0, 100),
                    GainDb = nativeChannel.GainDb,
                    ManualGainDb = prior?.ManualGainDb ?? nativeChannel.ManualGainDb ?? 0,
                    Pan = prior?.Pan ?? nativeChannel.Pan ?? 0,
                    Solo = prior?.Solo ?? nativeChannel.Solo,
                    // Zoom-garble fix: nativeChannel.NoiseSuppression is the ANALYZER
                    // heuristic (telemetry), not an operator setting - echoing it
                    // into the row made the core run the NS gate on Zoom sources,
                    // flickering per tick (attack-from-silence at every block seam).
                    // Suppression is operator-only: preserve the prior choice.
                    NoiseSuppression = prior?.NoiseSuppression ?? false,
                    Muted = prior?.Muted ?? nativeChannel.Muted,
                    Status = string.IsNullOrWhiteSpace(nativeChannel.Status) ? "native-pcm" : nativeChannel.Status,
                    Lufs = nativeChannel.RmsDbfs,
                    TruePeakDb = nativeChannel.PeakDbfs,
                    GainReductionDb = nativeChannel.GainReductionDb,
                    PluginInserts = prior?.PluginInserts.ToList() ??
                        nativeChannel.PluginInserts.Select(insert => insert.Name).ToList(),
                    InsertSettings = prior?.InsertSettings ?? new(StringComparer.OrdinalIgnoreCase)
                };

                mergedById[channel.ParticipantId] = channel;
            }
        }

        foreach (var sourceId in BuildExpectedAudioSourceIds())
        {
            if (mergedById.ContainsKey(sourceId))
            {
                continue;
            }

            var prior = existing.GetValueOrDefault(sourceId);
            mergedById[sourceId] = BuildWaitingForPcmAudioMixChannel(sourceId, prior);
        }

        _audioMixChannels.Clear();
        _audioMixChannels.AddRange(mergedById.Values);

        // C7d (owner: workspace controls silently dead): the processing
        // workspace edits the SELECTED channel — with no selection every
        // slider snapped back. Auto-select ONLY when the selection is EMPTY.
        // Crash lesson (0xc000027b, gate-toggle repro): stealing a selection
        // that merely LEFT the map for a tick (transient partial syncs) made
        // selection ping-pong at snapshot rate — rack list rebuild + ~20
        // workspace notifications per flip until the CoreMessagingXP
        // fail-fast. A momentarily-missing selection is HELD, mirroring the
        // core's hold-last rule: control-plane transients must not move UI
        // state either.
        if (mergedById.Count > 0 && string.IsNullOrEmpty(SelectedParticipantId))
        {
            SelectedParticipantId = mergedById.Keys.First();
        }
    }

    /// <summary>C7d: true when the workspace has a live channel to edit.</summary>
    public bool HasSelectedAudioChannel => SelectedAudioMix is not null;

    /// <summary>Workspace cells dim when there is nothing to edit (Grid has no IsEnabled).</summary>
    public double WorkspaceEnabledOpacity => HasSelectedAudioChannel ? 1.0 : 0.45;

    public static ParticipantAudioMix BuildWaitingForPcmAudioMixChannel(
        string sourceId,
        ParticipantAudioMix? prior) =>
        new()
        {
            ParticipantId = sourceId,
            OutputLevel = 0,
            GainDb = 0,
            ManualGainDb = prior?.ManualGainDb ?? 0,
            Pan = prior?.Pan ?? 0,
            Solo = prior?.Solo ?? false,
            NoiseSuppression = prior?.NoiseSuppression ?? false,
            Muted = prior?.Muted ?? false,
            Status = "waiting-for-pcm",
            Lufs = -120,
            TruePeakDb = -120,
            PluginInserts = prior?.PluginInserts.ToList() ?? [],
            InsertSettings = prior?.InsertSettings ?? new(StringComparer.OrdinalIgnoreCase)
        };

    private string BuildAutoProductionReadout()
    {
        var auto = _automationRecommendation;
        return auto.Confidence > 0
            ? $"Auto: {auto.Action} {auto.Confidence}% — {auto.Reason}"
            : auto.Reason;
    }

    private Dictionary<string, object?> BuildSpinePayload()
    {
        var syncContext = BuildProductionSyncContext();
        var participants = syncContext.Participants;
        if (_bridge.LastSnapshot?.MeetingState?.Equals("in_meeting", StringComparison.Ordinal) == true &&
            _bridge.LastSnapshot.Participants is { Count: > 0 } liveParticipants)
        {
            participants = liveParticipants
                .Select(participant => new MediaCoreParticipantWire(
                    participant.UserId,
                    participant.DisplayName,
                    participant.Role ?? "guest",
                    participant.BreakoutRoomId ?? _currentRoomId,
                    participant.BreakoutRoomName ?? _currentRoomName,
                    string.Equals(participant.UserId, _bridge.LastSnapshot.ActiveSpeakerId, StringComparison.Ordinal) ||
                    participant.Talking == true,
                    participant.Muted == true,
                    participant.SharingScreen == true,
                    participant.AudioLevel ?? 0,
                    participant.NetworkQuality ?? "live"))
                .ToList();
        }

        // Carry the multiview layout on this frequent, reliable channel. The production sync
        // (BuildSyncCommands → set-multiview-layout) only fires on user actions (Take, scene
        // change, Engine toggle) and sends an EMPTY layout at startup, so the GPU multiview never
        // gets the live roster from it. The spine sync runs continuously, so the core applies the
        // layout here (deduped by signature core-side). syncContext already built the sources/dims.
        var multiview = MultiviewGpuEnabled
            ? new MediaCoreMultiviewLayout(
                syncContext.MultiviewCanvasWidth,
                syncContext.MultiviewCanvasHeight,
                syncContext.MultiviewColumns,
                syncContext.MultiviewRows,
                syncContext.MultiviewSources)
            : null;

        return ZoomMediaSpinePayloadBuilder.Build(
            new ZoomMediaSpinePayloadBuilder.BuildInput
            {
                Participants = participants,
                Recording = Recording,
                SelectedBreakoutRoomId = _currentRoomId,
                EngineRunning = ZoomCaptureSubscribed,
                // Explicit opt-in: raw capture (and the Zoom recording-rights
                // request) only starts when the operator turns Engine On.
                StartCapture = ZoomCaptureSubscribed,
                OAuthSignedIn = Settings.ZoomOAuthSignedIn,
                SdkVersion = _bridge.Profile?.Name ?? "zoom-engine",
                SdkRuntimeReady = !Settings.SdkIsBlocked,
                Multiview = multiview
            });
    }

    private async Task<NativeMediaCoreStateSnapshot> SyncActiveSceneAsync(string? reason = null)
    {
        var scene = Scenes.First(s => s.Id == ActiveSceneId);
        var syncContext = BuildProductionSyncContext();
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(syncContext);
        if (!string.IsNullOrWhiteSpace(reason))
        {
            LaunchLog.Write(
                $"media-core sync batch reason={reason} " +
                $"recording={syncContext.Recording} streaming={syncContext.Streaming} " +
                $"destinations={FormatStreamDestinationTelemetry(syncContext.StreamDestinations)} " +
                $"commands={string.Join(",", commands.Select(command => command.Type))}");
        }
        // ConfigureAwait(true): resume on the CALLER'S context. UI callers (Engine On/Off,
        // command handlers) then run ApplyLiveProductionPatch on the real binding thread,
        // which is more reliable than the captured _dispatcher (the recurring off-thread
        // set_OutputStatus crash came through here in a re-entrant Engine-On path where the
        // guard's HasThreadAccess was bypassed). Off-thread callers have no UI context, so
        // they resume on the pool and ApplyLiveProductionPatch's own guard marshals them.
        var snapshot = await _bridge.SyncAsync(commands).ConfigureAwait(true);
        ApplyLiveProductionPatch(LiveProductionSync.MapSnapshotToStudioPatch(snapshot, BuildLiveProductionContext()));
        RunOnUiThread(() => CommandStatus = $"{scene.Name} synced to media core");
        return snapshot;
    }

    private MediaCoreProductionSyncContext BuildProductionSyncContext()
    {
        var resolvedSceneProgramRoutes = GetMutableRoutes(ActiveSceneId)
            .Select(ResolveRouteFromShowInput)
            .ToList();
        var resolvedProgramRoutes = BrowserOverlayProgramService.ApplyKeyState(
                ActiveSceneId,
                resolvedSceneProgramRoutes,
                _onAirBrowserOverlayIds)
            .ToList();
        var sceneRoutes = resolvedProgramRoutes
            .Select(route => BuildSceneRouteWire(route, resolvedProgramRoutes))
            .ToList();

        // PREVIEW scene graph (same wire shape as the program scene), so the core composites
        // the full previewed scene into its own preview shared texture. Resolved the same way
        // as the program routes; the core skips the extra composite for single-source previews.
        var resolvedScenePreviewRoutes = GetPreviewEditableRoutes()
            .Select(ResolveRouteFromShowInput)
            .ToList();
        var resolvedPreviewRoutes = BrowserOverlayProgramService.ApplyKeyState(
                PreviewSceneId,
                resolvedScenePreviewRoutes,
                _previewBrowserOverlayIds)
            .ToList();
        var previewSceneRoutes = resolvedPreviewRoutes
            .Select(route => BuildSceneRouteWire(route, resolvedPreviewRoutes))
            .ToList();

        var participants = RoomVideoParticipants
            .Select(participant => new MediaCoreParticipantWire(
                participant.Id,
                participant.Name,
                // R1: an operator-assigned production role outranks the
                // Zoom-derived one on the wire, so the core director/automation
                // reasons in show roles ("reader speaking"), not Zoom flags.
                _participantProductionRoles.TryGetValue(participant.Id, out var productionRole)
                    ? productionRole
                    : participant.Role.ToString().ToLowerInvariant(),
                participant.BreakoutRoomId,
                participant.BreakoutRoomName,
                participant.IsActiveSpeaker,
                participant.IsMuted,
                participant.IsScreenSharing,
                participant.AudioLevel,
                MapParticipantHealth(participant.Health)))
            .ToList();

        var audioRoutingSends = AudioRoutingMatrix.Rows
            .SelectMany(row => row.Cells)
            .Where(cell => cell.IsRouted)
            .Select(cell => new MediaCoreAudioRoutingSendWire(
                ResolveAudioRoutingMatrixSourceId(cell.SourceId),
                cell.Bus.Id,
                cell.GainDb,
                ResolveAudioProcessingInserts(FormatBusProcessingTargetId(cell.Bus.Id))))
            .ToList();

        var captureAudioSources = CaptureDevices
            .Where(device => !string.Equals(device.Vendor, "srt", StringComparison.OrdinalIgnoreCase))
            .Select(BuildCaptureAudioSourceWire)
            .Where(IsConfiguredCaptureAudioSource)
            .ToList();
        if (LocalAudioSourceEnabled &&
            ResolveSelectedLocalAudioCaptureDevice(
                SelectedLocalAudioCaptureDeviceId,
                AudioCaptureDevices) is { } localAudio)
        {
            captureAudioSources.Add(new MediaCoreCaptureAudioSourceWire(
                "local-machine-audio",
                localAudio.Id,
                localAudio.DisplayLabel,
                0,
                localAudio.SourceKind,
                localAudio.NativeDeviceId,
                localAudio.DriverName,
                localAudio.IsEmbeddedCaptureAudio));
        }

        audioRoutingSends = EnsureDefaultCaptureAudioRoutingSends(
            audioRoutingSends,
            captureAudioSources)
            .ToList();
        audioRoutingSends = EnsureDefaultMediaAudioRoutingSends(
            audioRoutingSends,
            ResolveSceneMediaAudioSourceIds(resolvedProgramRoutes).Count > 0)
            .ToList();
        audioRoutingSends = EnsureDefaultZoomAudioRoutingSends(
            audioRoutingSends,
            RoomParticipantsForInputs.Select(participant => participant.Id).ToList())
            .ToList();
        // Default/fallback sends are synthesized after the matrix rows above.
        // Attach each bus's insert chain here so MASTER VST processing remains
        // live even when a source is using an implicit default route.
        audioRoutingSends = audioRoutingSends
            .Select(send => send with
            {
                BusPluginInserts = ResolveAudioProcessingInserts(FormatBusProcessingTargetId(send.BusId))
            })
            .ToList();

        RefreshAudioMixChannels();
        var audioChannels = BuildAudioMixChannelWires(captureAudioSources, audioRoutingSends);

        var isoParticipantIds = BuildIsoParticipantTargets();

        if (isoParticipantIds.Count == 0)
        {
            isoParticipantIds = MediaCoreProductionSyncContext.DefaultRecordingTargets.IsoParticipantIds.ToList();
        }

        var playbackSelection = MediaRoutePlaybackService.ResolvePlaybackSelection(
            SelectedMediaAssetId,
            SelectedMediaAssetPlaying,
            resolvedProgramRoutes);
        var selectedMediaAsset = playbackSelection.MediaAssetId is null ? null : FindMediaAsset(playbackSelection.MediaAssetId);
        var selectedMediaPlaybackKey = selectedMediaAsset is null
            ? null
            : MediaRoutePlaybackService.BuildSceneMediaPlaybackKey(
                selectedMediaAsset.Id,
                isProgramScene: MediaRoutePlaybackService.IsMediaAssetRoutedOnProgram(selectedMediaAsset.Id, resolvedProgramRoutes),
                _programMediaPlaybackTakeVersion);

        var canvasProfile = BuildRequestedOutputProfile("canvas", CanvasResolution, CanvasFps, "h264");
        // The ordered Show Input roster that drives the core-composited GPU multiview. Built from
        // the SAME slots that feed ShowInputRosterService.BuildMultiviewTiles, so the multiview
        // monitor and the Sources roster always agree on which sources are live.
        var multiviewSources = MultiviewGpuEnabled
            ? ShowInputRosterService.BuildMultiviewLayoutSources(
                ShowInputs,
                RoomParticipantsForInputs,
                CaptureDevices,
                VisualMediaAssets,
                _sourceDisplayNames)
            : [];
        var multiviewGrid = ShowInputRosterService.ResolveGridShape(multiviewSources.Count);

        return new MediaCoreProductionSyncContext
        {
            ActiveSceneId = ActiveSceneId,
            SceneRoutes = sceneRoutes,
            SceneBackground = BuildSceneBackgroundWire(ActiveSceneId),
            PreviewSceneId = PreviewSceneId,
            PreviewSceneRoutes = previewSceneRoutes,
            PreviewSceneBackground = BuildSceneBackgroundWire(PreviewSceneId),
            PreviewColorGrade = new MediaCoreColorGradeWire(
                ColorGrade.Lut,
                ColorGrade.Exposure,
                ColorGrade.Contrast,
                ColorGrade.Saturation,
                ColorGrade.Temperature),
            MultiviewSources = multiviewSources,
            MultiviewCanvasWidth = canvasProfile.Width,
            MultiviewCanvasHeight = canvasProfile.Height,
            MultiviewColumns = multiviewGrid.Columns,
            MultiviewRows = multiviewGrid.Rows,
            Participants = participants,
            Recording = Recording,
            Streaming = Streaming,
            StreamDestinations = BuildSelectedStreamDestinations(validatedOnly: true),
            StreamDestinationSettings = BuildStreamDestinationSettings(),
            SrtIngestSources = BuildSrtIngestSourceSettings(),
            CanvasOutputProfile = canvasProfile,
            StreamOutputProfile = BuildRequestedOutputProfile(
                "stream",
                StreamRenderResolution,
                StreamRenderFps,
                StreamVideoCodec,
                NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps),
                NormalizeAudioBitrateKbps(StreamAudioBitrateKbps)),
            RecordingOutputProfile = BuildRequestedOutputProfile(
                "recording",
                RecordingRenderResolution,
                RecordingRenderFps,
                RecordingVideoCodec,
                NormalizeOutputTargetBitrateMbps(RecordingTargetBitrateMbps),
                NormalizeAudioBitrateKbps(RecordingAudioBitrateKbps)),
            RecordingTargets = BuildRecordingTargets(isoParticipantIds),
            Graphics = Graphics
                .Select(graphic => new MediaCoreGraphicWire(
                    graphic.Id,
                    graphic.Name,
                    graphic.Position,
                    graphic.Enabled,
                    graphic.Kind))
                .ToList(),
            LowerThirdKey = ProgramLowerThirdKey.IsVisible
                ? new MediaCoreLowerThirdKeyWire(
                    ProgramLowerThirdKey.SourceId,
                    ProgramLowerThirdKey.SourceName,
                    ProgramLowerThirdKey.Title,
                    ProgramLowerThirdKey.Org,
                    ProgramLowerThirdKey.Position,
                    ProgramLowerThirdKey.Phase,
                    ProgramLowerThirdKey.Enabled,
                    ProgramLowerThirdKey.BuildInMs,
                    ProgramLowerThirdKey.BuildOutMs)
                : null,
            ColorGrade = new MediaCoreColorGradeWire(
                ColorGrade.Lut,
                ColorGrade.Exposure,
                ColorGrade.Contrast,
                ColorGrade.Saturation,
                ColorGrade.Temperature),
            BrandKit = new MediaCoreBrandKitWire(
                BrandKit.Name,
                BrandKit.LogoText,
                BrandKit.LogoAssetId,
                BrandKit.LogoAssetName,
                BrandKit.LogoAssetPath,
                BrandKit.BrandColor,
                BrandKit.AccentColor,
                BrandKit.BackgroundColor,
                BrandKit.FontFamily,
                BrandKit.LowerThirdStyle,
                BrandKit.CaptionStyle,
                BrandKit.DefaultOverlayBehavior),
            AudioLimiterEnabled = MasterLimiterEnabled,
            AudioMasteringEnabled = MasteringEnabled,
            AudioMasteringTargetLufs = MasteringTargetLufs,
            AudioMasteringGlueAmount = MasteringGlueAmount,
            AudioMasteringCeilingDbfs = MasteringCeilingDbfs,
            AudioMasteringMaxRideDb = MasteringMaxRideDb,
            AudioMasteringInputGainDb = MasteringInputGainDb,
            AudioMasteringHighPassHz = MasteringHighPassHz,
            AudioMasteringLowPassHz = MasteringLowPassHz,
            AudioMasteringLowShelfDb = MasteringLowShelfDb,
            AudioMasteringPresenceDb = MasteringPresenceDb,
            AudioMasteringHighShelfDb = MasteringHighShelfDb,
            AudioMasteringStereoWidth = MasteringStereoWidth,
            VirtualCameraEnabled = VirtualCameraEnabled,
            VirtualCameraMirror = VirtualCameraMirror,
            VirtualCameraDeviceName = VirtualCameraDeviceName,
            ScanVstPlugins = ConsumeVstScanRequest(),
            AudioMonitor = new MediaCoreAudioMonitorWire(
                AudioMonitoringEnabled,
                SelectedAudioMonitorNativeDeviceId,
                SelectedAudioMonitorDeviceName,
                AudioMonitorVolume),
            AudioMixChannels = audioChannels,
            AudioRoutingSends = audioRoutingSends,
            AudioBusSends = AudioRoutingMatrix.BuildBusSends()
                .Select(send => new MediaCoreAudioBusSendWire(send.FromBusId, send.ToBusId, send.GainDb))
                .ToList(),
            AudioMonitorListenBusId = AudioRoutingMatrix.MonitorListenBusId,
            CaptureAudioSources = captureAudioSources,
            CaptionText = CaptionText,
            CaptionSpeaker = CaptionSpeaker,
            SelectedMediaAssetId = selectedMediaAsset?.Id,
            SelectedMediaAssetName = selectedMediaAsset?.Name,
            SelectedMediaAssetKind = selectedMediaAsset?.Kind,
            SelectedMediaAssetPath = selectedMediaAsset?.FilePath,
            SelectedMediaPlaybackKey = selectedMediaPlaybackKey,
            SelectedMediaAssetPlaying = selectedMediaAsset is not null && playbackSelection.Playing
        };
    }

    private MediaCoreSceneRouteWire BuildSceneRouteWire(
        SourceRoute route,
        IReadOnlyList<SourceRoute> programRoutes)
    {
        var mediaAsset = TryResolveRouteMediaAsset(route);
        var mediaPlayback = mediaAsset is null
            ? null
            : MediaRoutePlaybackService.ResolveSceneRoutePlayback(
                mediaAsset.Id,
                isProgramScene: true,
                SelectedMediaAssetId,
                SelectedMediaAssetPlaying,
                programRoutes,
                _programMediaPlaybackTakeVersion);
        return new MediaCoreSceneRouteWire(
            route.Id,
            SceneRoutingService.ModeToWire(route.Mode),
            SceneRoutingService.AudioRoleToWire(route.AudioRole),
            route.ParticipantId,
            route.CanvasRect?.X,
            route.CanvasRect?.Y,
            route.CanvasRect?.Width,
            route.CanvasRect?.Height,
            route.ZIndex,
            CaptureDeviceId: route.CaptureDeviceId,
            FitMode: route.FitMode,
            BorderStyle: route.BorderStyle,
            BorderColor: route.BorderColor,
            BorderThickness: route.BorderThickness,
            SourceScale: route.SourceScale,
            SourceOffsetX: route.SourceOffsetX,
            SourceOffsetY: route.SourceOffsetY,
            Opacity: route.Opacity,
            ColorGrade: BuildRouteColorGradeWire(route),
            MediaAssetId: mediaAsset?.Id,
            MediaAssetName: mediaAsset?.Name,
            MediaAssetKind: mediaAsset?.Kind,
            MediaAssetPath: mediaAsset?.FilePath,
            MediaPlaybackKey: mediaPlayback?.MediaPlaybackKey,
            MediaAssetPlaying: mediaPlayback?.Playing == true);
    }

    private MediaCoreCaptureAudioSourceWire BuildCaptureAudioSourceWire(CaptureDevice captureDevice)
    {
        var audioDevice = string.IsNullOrWhiteSpace(captureDevice.AssignedAudioDeviceId)
            ? null
            : AudioCaptureDevices.FirstOrDefault(device =>
                string.Equals(device.Id, captureDevice.AssignedAudioDeviceId, StringComparison.Ordinal));

        return new MediaCoreCaptureAudioSourceWire(
            captureDevice.Id,
            audioDevice?.Id ?? captureDevice.AssignedAudioDeviceId,
            audioDevice?.DisplayLabel ?? captureDevice.AssignedAudioDeviceName,
            captureDevice.AudioSyncOffsetMs,
            audioDevice?.SourceKind ?? "none",
            audioDevice?.NativeDeviceId,
            audioDevice?.DriverName,
            audioDevice?.IsEmbeddedCaptureAudio ?? false);
    }

    private List<MediaCoreAudioMixChannelWire> BuildAudioMixChannelWires(
        IReadOnlyList<MediaCoreCaptureAudioSourceWire> captureAudioSources,
        IReadOnlyList<MediaCoreAudioRoutingSendWire> audioRoutingSends)
    {
        var audioMixByParticipant = AudioMix.Participants
            .Where(mix => !string.IsNullOrWhiteSpace(mix.ParticipantId))
            .GroupBy(mix => mix.ParticipantId, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.Last(), StringComparer.Ordinal);
        var participantsById = RoomVideoParticipants
            .Where(participant => !string.IsNullOrWhiteSpace(participant.Id))
            .GroupBy(participant => participant.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.Last(), StringComparer.Ordinal);

        return BuildExpectedAudioSourceIds(captureAudioSources, audioRoutingSends)
            .Select(sourceId => BuildAudioMixChannelWire(sourceId, audioMixByParticipant, participantsById))
            .ToList();
    }

    private IReadOnlyList<string> BuildExpectedAudioSourceIds(
        IReadOnlyList<MediaCoreCaptureAudioSourceWire>? captureAudioSources = null,
        IReadOnlyList<MediaCoreAudioRoutingSendWire>? audioRoutingSends = null)
    {
        var sourceIds = new List<string>();
        sourceIds.AddRange(RoomVideoParticipants.Select(participant => participant.Id));
        sourceIds.AddRange(_audioMixChannels.Select(channel => channel.ParticipantId));
        sourceIds.AddRange(ResolveSceneMediaAudioSourceIds(PreviewSceneRoutes));
        sourceIds.AddRange(ResolveSceneMediaAudioSourceIds(ProgramSceneRoutes));

        if (captureAudioSources is not null)
        {
            sourceIds.AddRange(captureAudioSources
                .Where(IsConfiguredCaptureAudioSource)
                .Select(ResolveCaptureAudioChannelId));
        }
        else
        {
            sourceIds.AddRange(CaptureDevices
                .Where(device => !string.Equals(device.Vendor, "srt", StringComparison.OrdinalIgnoreCase))
                .Where(device => !string.IsNullOrWhiteSpace(device.AssignedAudioDeviceId) ||
                    !string.IsNullOrWhiteSpace(device.AssignedAudioDeviceName))
                .Select(device => $"capture:{device.Id}"));

            if (IsLocalAudioSourceConfigured(LocalAudioSourceEnabled, SelectedLocalAudioCaptureDeviceId, AudioCaptureDevices))
            {
                sourceIds.Add("local-machine-audio");
            }
        }

        if (audioRoutingSends is not null)
        {
            sourceIds.AddRange(audioRoutingSends
                .Select(send => send.SourceId)
                .Where(IsConcreteAudioMixSourceId));
        }
        else
        {
            sourceIds.AddRange(AudioRoutingMatrix.Rows
                .Where(row => row.Cells.Any(cell => cell.IsRouted))
                .Select(row => ResolveAudioRoutingMatrixSourceId(row.SourceId))
                .Where(IsConcreteAudioMixSourceId));
        }

        return sourceIds
            .Where(sourceId => !string.IsNullOrWhiteSpace(sourceId))
            .Distinct(StringComparer.Ordinal)
            .ToList();
    }

    public static IReadOnlyList<string> ResolveSceneMediaAudioSourceIds(IEnumerable<SourceRoute> routes)
    {
        return routes
            .Where(route => route.Mode == SourceRouteMode.Fixed &&
                ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out _))
            .Select(_ => "media")
            .Distinct(StringComparer.Ordinal)
            .ToList();
    }

    public static IReadOnlyList<MediaCoreAudioRoutingSendWire> EnsureDefaultLocalAudioRoutingSends(
        IReadOnlyList<MediaCoreAudioRoutingSendWire> sends,
        bool localAudioConfigured,
        bool localRoutingRowExists)
    {
        _ = localRoutingRowExists;
        if (!localAudioConfigured)
        {
            return sends;
        }

        string[] requiredBusIds = ["master", "pgm-l", "pgm-r", "stream", "mon"];
        var completed = sends.ToList();
        foreach (var busId in requiredBusIds)
        {
            if (!completed.Any(send =>
                    string.Equals(send.SourceId, "local-machine-audio", StringComparison.Ordinal) &&
                    string.Equals(send.BusId, busId, StringComparison.OrdinalIgnoreCase)))
            {
                completed.Add(new MediaCoreAudioRoutingSendWire("local-machine-audio", busId, 0));
            }
        }

        return completed;
    }

    public static IReadOnlyList<MediaCoreAudioRoutingSendWire> EnsureDefaultCaptureAudioRoutingSends(
        IReadOnlyList<MediaCoreAudioRoutingSendWire> sends,
        IReadOnlyList<MediaCoreCaptureAudioSourceWire> captureAudioSources)
    {
        var completed = sends.ToList();
        string[] requiredBusIds = ["master", "pgm-l", "pgm-r", "stream", "mon"];
        var sourceIds = captureAudioSources
            .Where(IsConfiguredCaptureAudioSource)
            .Select(ResolveCaptureAudioChannelId)
            .Where(sourceId => !string.IsNullOrWhiteSpace(sourceId))
            .Distinct(StringComparer.Ordinal);

        foreach (var sourceId in sourceIds)
        {
            foreach (var busId in requiredBusIds)
            {
                if (!completed.Any(send =>
                        string.Equals(send.SourceId, sourceId, StringComparison.Ordinal) &&
                        string.Equals(send.BusId, busId, StringComparison.OrdinalIgnoreCase)))
                {
                    completed.Add(new MediaCoreAudioRoutingSendWire(sourceId, busId, 0));
                }
            }
        }

        return completed;
    }

    // Zoom participants deliver real ISO PCM (engine SHM ingest), but a channel
    // with NO crosspoint sends contributes to no bus — and the routed buses
    // shadow the fallback mixer monitor bus, so an unrouted participant is
    // silent on master/monitor/program/stream no matter how hot their meter
    // is. Default every in-room participant to unity sends on the same buses
    // capture/local/media sources get; the routing matrix can still override.
    // Z1 (docs/zoom-audio-spec.md): Zoom ISO participant streams default
    // UNROUTED - routing the same voice via BOTH the meeting mix and ISO
    // summed it twice at ~110ms skew (the measured internal echo). The
    // MEETING MIX ("zoom-mix") carries the program defaults instead; ISO rows
    // stay visible in the matrix for deliberate per-speaker routing.
    public static IReadOnlyList<MediaCoreAudioRoutingSendWire> EnsureDefaultZoomAudioRoutingSends(
        IReadOnlyList<MediaCoreAudioRoutingSendWire> sends,
        IReadOnlyList<string> zoomParticipantIds)
    {
        if (zoomParticipantIds.Count == 0)
        {
            return sends;
        }

        string[] requiredBusIds = ["master", "pgm-l", "pgm-r", "stream", "mon"];
        var completed = sends.ToList();
        foreach (var busId in requiredBusIds)
        {
            if (!completed.Any(send =>
                    string.Equals(send.SourceId, "zoom-mix", StringComparison.Ordinal) &&
                    string.Equals(send.BusId, busId, StringComparison.OrdinalIgnoreCase)))
            {
                completed.Add(new MediaCoreAudioRoutingSendWire("zoom-mix", busId, 0));
            }
        }

        return completed;
    }

    public static IReadOnlyList<MediaCoreAudioRoutingSendWire> EnsureDefaultMediaAudioRoutingSends(
        IReadOnlyList<MediaCoreAudioRoutingSendWire> sends,
        bool mediaAudioConfigured)
    {
        if (!mediaAudioConfigured)
        {
            return sends;
        }

        string[] requiredBusIds = ["master", "pgm-l", "pgm-r", "stream", "mon"];
        var completed = sends.ToList();
        foreach (var busId in requiredBusIds)
        {
            if (!completed.Any(send =>
                    string.Equals(send.SourceId, "media", StringComparison.Ordinal) &&
                    string.Equals(send.BusId, busId, StringComparison.OrdinalIgnoreCase)))
            {
                completed.Add(new MediaCoreAudioRoutingSendWire("media", busId, 0));
            }
        }

        return completed;
    }

    private MediaCoreAudioMixChannelWire BuildAudioMixChannelWire(
        string sourceId,
        IReadOnlyDictionary<string, ParticipantAudioMix> audioMixByParticipant,
        IReadOnlyDictionary<string, Participant> participantsById)
    {
        audioMixByParticipant.TryGetValue(sourceId, out var mix);
        participantsById.TryGetValue(sourceId, out var participant);

        var manualGain = NormalizeMixerGain(mix?.ManualGainDb ?? 0);
        return new MediaCoreAudioMixChannelWire(
            sourceId,
            Math.Clamp(participant?.AudioLevel ?? mix?.OutputLevel ?? 0, 0, 100),
            participant?.IsMuted ?? mix?.Muted ?? false,
            mix?.NoiseSuppression ?? false,
            Math.Abs(manualGain) < 0.05 ? null : manualGain,
            NormalizeMixerPan(mix?.Pan ?? 0),
            mix?.Solo ?? false,
            mix?.PluginInserts ?? [],
            mix?.InsertSettings);
    }

    public static bool IsConfiguredCaptureAudioSource(MediaCoreCaptureAudioSourceWire source) =>
        source.Embedded ||
        !string.IsNullOrWhiteSpace(source.AudioDeviceId) ||
        !string.IsNullOrWhiteSpace(source.NativeAudioDeviceId) ||
        string.Equals(source.CaptureDeviceId, "local-machine-audio", StringComparison.Ordinal);

    public static bool IsLocalAudioSourceConfigured(
        bool localAudioSourceEnabled,
        string? selectedLocalAudioCaptureDeviceId,
        IEnumerable<AudioCaptureDevice> audioCaptureDevices) =>
        localAudioSourceEnabled &&
        !string.IsNullOrWhiteSpace(selectedLocalAudioCaptureDeviceId) &&
        (IsDefaultRenderLoopbackSelection(selectedLocalAudioCaptureDeviceId) ||
            audioCaptureDevices.Any(device =>
                string.Equals(device.Id, selectedLocalAudioCaptureDeviceId, StringComparison.Ordinal)));

    private static AudioCaptureDevice? ResolveSelectedLocalAudioCaptureDevice(
        string? selectedLocalAudioCaptureDeviceId,
        IEnumerable<AudioCaptureDevice> audioCaptureDevices)
    {
        if (string.IsNullOrWhiteSpace(selectedLocalAudioCaptureDeviceId))
        {
            return null;
        }

        var matchedDevice = audioCaptureDevices.FirstOrDefault(device =>
            string.Equals(device.Id, selectedLocalAudioCaptureDeviceId, StringComparison.Ordinal));
        return matchedDevice ?? (IsDefaultRenderLoopbackSelection(selectedLocalAudioCaptureDeviceId)
            ? AudioCaptureDeviceDiscoveryService.CreateDefaultLoopbackFallbackDevice()
            : null);
    }

    private static bool IsDefaultRenderLoopbackSelection(string? selectedLocalAudioCaptureDeviceId) =>
        string.Equals(
            selectedLocalAudioCaptureDeviceId,
            "default-render-loopback",
            StringComparison.OrdinalIgnoreCase);

    private static string ResolveCaptureAudioChannelId(MediaCoreCaptureAudioSourceWire source) =>
        string.Equals(source.CaptureDeviceId, "local-machine-audio", StringComparison.Ordinal)
            ? "local-machine-audio"
            : $"capture:{source.CaptureDeviceId}";

    public static bool IsConcreteAudioMixSourceId(string sourceId) =>
        !string.Equals(sourceId, "zoom-mix", StringComparison.OrdinalIgnoreCase) &&
        !string.Equals(sourceId, "active-speaker", StringComparison.OrdinalIgnoreCase) &&
        !string.Equals(sourceId, "screen-share", StringComparison.OrdinalIgnoreCase);

    private IReadOnlyList<MediaCoreSrtIngestSourceWire> BuildSrtIngestSourceSettings() =>
        SrtIngestSources
            .Select(source => new MediaCoreSrtIngestSourceWire(
                source.Id,
                source.DeviceId,
                source.Name,
                NormalizeOutputText(source.Mode, "listener"),
                NormalizeOutputText(source.Host, "0.0.0.0"),
                ParsePositiveInt(source.Port) ?? 10000,
                ParsePositiveInt(source.LatencyMs) ?? 120,
                NormalizeOptionalOutputText(source.StreamId),
                string.IsNullOrWhiteSpace(source.Passphrase) ? null : source.Passphrase))
            .ToList();

    private IReadOnlyList<string> BuildSelectedStreamDestinations(bool validatedOnly = false)
    {
        var destinations = new List<string>(3);
        if (StreamRtmpEnabled &&
            (!validatedOnly || StudioStreamOutputValidation.CanSerializeRtmpSettings(
                StreamRtmpProtocol,
                StreamRtmpServerUrl,
                StreamRtmpStreamKey)))
        {
            destinations.Add("rtmp");
        }

        if (StreamNdiEnabled &&
            (!validatedOnly || StudioStreamOutputValidation.CanSerializeNdiSettings(StreamNdiProgramName)))
        {
            destinations.Add("ndi");
        }

        if (StreamSrtEnabled &&
            (!validatedOnly || StudioStreamOutputValidation.CanSerializeSrtSettings(
                StreamSrtMode,
                StreamSrtHost,
                StreamSrtPort,
                StreamSrtLatencyMs,
                StreamSrtStreamId,
                StreamSrtKeyLength,
                StreamSrtPassphrase)))
        {
            destinations.Add("srt");
        }

        return destinations;
    }

    private IReadOnlyList<MediaCoreStreamDestinationWire> BuildStreamDestinationSettings()
    {
        var destinations = new List<MediaCoreStreamDestinationWire>(3);
        var streamProfile = BuildRequestedOutputProfile(
            "stream",
            StreamRenderResolution,
            StreamRenderFps,
            StreamVideoCodec,
            NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps),
            NormalizeAudioBitrateKbps(StreamAudioBitrateKbps));
        if (StreamRtmpEnabled &&
            StudioStreamOutputValidation.CanSerializeRtmpSettings(
                StreamRtmpProtocol,
                StreamRtmpServerUrl,
                StreamRtmpStreamKey))
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "rtmp",
                Label: "RTMP",
                Protocol: StudioStreamOutputValidation.NormalizeRtmpProtocol(StreamRtmpProtocol),
                Url: StudioStreamOutputValidation.BuildRtmpUrl(StreamRtmpProtocol, StreamRtmpServerUrl),
                StreamKey: NormalizeOutputText(StreamRtmpStreamKey, string.Empty),
                FfmpegBinDirectory: NormalizeOptionalOutputText(FfmpegBinDirectory),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                AudioBitrateKbps: streamProfile.AudioBitrateKbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode),
                KeyframeIntervalSeconds: Math.Clamp(StreamKeyframeIntervalSeconds, 0.5, 10),
                RateControl: NormalizeStreamRateControl(StreamRateControl),
                H264Profile: NormalizeStreamH264Profile(StreamH264Profile),
                BFrames: (int)Math.Clamp(Math.Round(StreamBFrames), 0, 4),
                AllowEnhancedRtmp: StreamAllowEnhancedRtmp));
        }

        if (StreamNdiEnabled &&
            StudioStreamOutputValidation.CanSerializeNdiSettings(StreamNdiProgramName))
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "ndi",
                Label: "NDI",
                NdiName: NormalizeOutputText(StreamNdiProgramName, "CoreVideo Pro Program"),
                NdiGroup: NormalizeOutputText(StreamNdiGroupName, "public"),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                AudioBitrateKbps: streamProfile.AudioBitrateKbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode)));
        }

        if (StreamSrtEnabled &&
            StudioStreamOutputValidation.CanSerializeSrtSettings(
                StreamSrtMode,
                StreamSrtHost,
                StreamSrtPort,
                StreamSrtLatencyMs,
                StreamSrtStreamId,
                StreamSrtKeyLength,
                StreamSrtPassphrase))
        {
            var latencyMs = ParsePositiveInt(StreamSrtLatencyMs);
            var keyLength = StudioStreamOutputValidation.ParseSrtKeyLength(StreamSrtKeyLength);
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "srt",
                Label: "SRT",
                Mode: StudioStreamOutputValidation.NormalizeSrtMode(StreamSrtMode),
                Host: NormalizeOutputText(StreamSrtHost, string.Empty),
                Port: ParsePositiveInt(StreamSrtPort),
                LatencyMs: latencyMs,
                LatencyUs: latencyMs is null ? null : latencyMs * 1000,
                Passphrase: NormalizeOutputText(StreamSrtPassphrase, string.Empty),
                KeyLength: keyLength,
                StreamId: NormalizeOptionalOutputText(StreamSrtStreamId),
                Fps: streamProfile.Fps,
                TargetBitrateMbps: streamProfile.TargetBitrateMbps,
                VideoCodec: streamProfile.Codec,
                EncoderMode: NormalizeStreamEncoderMode(StreamEncoderMode)));
        }

        return destinations;
    }

    private static string FormatStreamDestinationTelemetry(IReadOnlyList<string> destinations) =>
        destinations.Count == 0 ? "none" : string.Join(",", destinations);

    private IReadOnlyList<string> BuildConfiguredStreamDestinationLabels()
    {
        var destinations = new List<string>(3);
        if (StreamRtmpEnabled)
        {
            destinations.Add(IsRtmpConfigured() ? "RTMP configured" : "RTMP missing");
        }

        if (StreamNdiEnabled)
        {
            destinations.Add(IsNdiConfigured() ? "NDI configured" : "NDI missing");
        }

        if (StreamSrtEnabled)
        {
            destinations.Add(ValidateSrtSettings() is null ? "SRT configured" : "SRT missing");
        }

        return destinations;
    }

    private string? ValidateStreamDestinations()
    {
        var settingsError = StudioStreamOutputValidation.ValidateSelectedDestinations(
            StreamRtmpEnabled,
            StreamNdiEnabled,
            StreamSrtEnabled,
            StreamRtmpProtocol,
            StreamRtmpServerUrl,
            StreamRtmpStreamKey,
            StreamNdiProgramName,
            StreamSrtMode,
            StreamSrtHost,
            StreamSrtPort,
            StreamSrtLatencyMs,
            StreamSrtStreamId,
            StreamSrtKeyLength,
            StreamSrtPassphrase,
            FfmpegBinDirectory);
        return settingsError ?? ValidateStreamDestinationCapabilities(
            StreamRtmpEnabled,
            StreamNdiEnabled,
            StreamSrtEnabled,
            _bridge.Profile);
    }

    public static string? ValidateStreamDestinationCapabilities(
        bool streamRtmpEnabled,
        bool streamNdiEnabled,
        bool streamSrtEnabled,
        NativeMediaCoreProfile? profile)
    {
        if (profile is null)
        {
            return null;
        }

        if (streamRtmpEnabled && !HasNativeOutputCapability(profile, "rtmp-output"))
        {
            return "RTMP output is selected, but the native media core profile is missing rtmp-output.";
        }

        if (streamNdiEnabled && !HasNativeOutputCapability(profile, "ndi-output"))
        {
            return "NDI output is selected, but the native media core profile is missing ndi-output.";
        }

        if (streamSrtEnabled && !HasNativeOutputCapability(profile, "srt-output"))
        {
            return "SRT output is selected, but the native media core profile is missing srt-output.";
        }

        return null;
    }

    private static bool HasNativeOutputCapability(NativeMediaCoreProfile profile, string capability) =>
        profile.Capabilities.Contains(capability, StringComparer.OrdinalIgnoreCase);

    private bool IsRtmpConfigured()
        => ValidateRtmpSettings() is null;

    private string? ValidateRtmpSettings() =>
        StudioStreamOutputValidation.ValidateRtmp(
            StreamRtmpProtocol,
            StreamRtmpServerUrl,
            StreamRtmpStreamKey);

    private bool IsNdiConfigured() => !string.IsNullOrWhiteSpace(StreamNdiProgramName);

    private string? ValidateSrtSettings()
        => StudioStreamOutputValidation.ValidateSrt(
            StreamSrtMode,
            StreamSrtHost,
            StreamSrtPort,
            StreamSrtLatencyMs,
            StreamSrtStreamId,
            StreamSrtKeyLength,
            StreamSrtPassphrase);

    private MediaCoreRecordingTargetsWire BuildRecordingTargets(IReadOnlyList<string> isoParticipantIds) =>
        new(
            TargetFolder: NormalizeOutputText(RecordingTargetFolder, ResolveDefaultRecordingFolder()),
            FilenamePrefix: NormalizeOutputText(RecordingFilenamePrefix, MediaCoreProductionSyncContext.DefaultRecordingTargets.FilenamePrefix),
            Format: NormalizeRecordingFormat(RecordingFormat),
            Quality: NormalizeRecordingQuality(RecordingQuality),
            IsoParticipantIds: isoParticipantIds.Take(8).ToList());

    private IReadOnlyList<string> BuildIsoParticipantTargets()
    {
        var ordered = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);

        foreach (var cell in VideoRoutingMatrix.Rows
            .SelectMany(row => row.Cells)
            .Where(cell => cell.IsRouted && cell.Destination.Id.StartsWith("iso-", StringComparison.OrdinalIgnoreCase))
            .OrderBy(cell => cell.Destination.Id, StringComparer.Ordinal))
        {
            if (TryResolveRoutingSourceParticipantId(cell.SourceId, out var participantId) &&
                seen.Add(participantId))
            {
                ordered.Add(participantId);
            }
        }

        foreach (var participantId in GetMutableRoutes(ActiveSceneId)
            .Where(route =>
                route.AudioRole == SourceAudioRole.Isolated &&
                route.ParticipantId is not null)
            .Select(route => route.ParticipantId!)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal))
        {
            if (seen.Add(participantId))
            {
                ordered.Add(participantId);
            }
        }

        return ordered.Take(8).ToList();
    }

    private bool TryResolveRoutingSourceParticipantId(string sourceId, out string participantId)
    {
        participantId = string.Empty;
        if (!TryParseInputSourceId(sourceId, out var slotNumber))
        {
            return false;
        }

        var slot = ShowInputs.FirstOrDefault(input => input.SlotNumber == slotNumber);
        if (slot?.Kind != ShowInputKind.ZoomParticipant ||
            string.IsNullOrWhiteSpace(slot.ParticipantId))
        {
            return false;
        }

        participantId = slot.ParticipantId;
        return true;
    }

    private static string NormalizeOutputText(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();

    private static string? NormalizeOptionalOutputText(string? value) =>
        string.IsNullOrWhiteSpace(value) ? null : value.Trim();

    private static int? ParsePositiveInt(string? value) =>
        int.TryParse(value, out var parsed) ? parsed : null;

    private static string NormalizeFpsText(string? value) =>
        Math.Clamp(ParsePositiveInt(value) ?? 60, 1, 240).ToString();

    private static MediaCoreOutputProfileWire BuildRequestedOutputProfile(
        string scope,
        string? resolution,
        string? fpsText,
        string? codec,
        double? targetBitrateMbps = null,
        int? audioBitrateKbps = null)
    {
        var normalizedResolution = NormalizeResolutionText(resolution);
        var parts = normalizedResolution.Split('x', StringSplitOptions.RemoveEmptyEntries);
        var width = parts.Length == 2 && int.TryParse(parts[0], out var parsedWidth) ? parsedWidth : 1920;
        var height = parts.Length == 2 && int.TryParse(parts[1], out var parsedHeight) ? parsedHeight : 1080;
        var fps = Math.Clamp(ParsePositiveInt(fpsText) ?? 60, 1, 240);
        var tier = height >= 2160 ? "4k" : $"{height}p";
        var profileId = $"{scope}-{tier}{fps}";

        return new MediaCoreOutputProfileWire(
            ProfileId: profileId,
            Resolution: $"{width}x{height}",
            Width: width,
            Height: height,
            Fps: fps,
            TargetBitrateMbps: targetBitrateMbps ?? EstimateTargetBitrateMbps(width, height, fps),
            AudioBitrateKbps: NormalizeAudioBitrateKbps(audioBitrateKbps ?? (scope == "recording" ? 192 : 160)),
            Codec: NormalizeVideoCodec(codec));
    }

    public static double NormalizeOutputTargetBitrateMbps(double value) =>
        Math.Round(double.IsFinite(value) ? Math.Clamp(value, 0.5, 80.0) : 8.2, 1, MidpointRounding.AwayFromZero);

    public static double NormalizeStreamTargetBitrateMbps(double value) =>
        NormalizeOutputTargetBitrateMbps(value);

    public static int NormalizeAudioBitrateKbps(double value) =>
        (int)Math.Round(double.IsFinite(value) ? Math.Clamp(value, 32, 512) : 160, MidpointRounding.AwayFromZero);

    public static double NormalizeAudioMonitorVolume(double value) =>
        Math.Round(double.IsFinite(value) ? Math.Clamp(value, 0.0, 1.0) : DefaultAudioMonitorVolume, 2, MidpointRounding.AwayFromZero);

    public static string FormatStreamBitrateSummary(double value)
    {
        var normalized = NormalizeStreamTargetBitrateMbps(value);
        return $"{normalized:0.0} Mbps ({normalized * 1000:0} kbps)";
    }

    public static string FormatTransportStreamConfigLabel(double targetBitrateMbps) =>
        $"{NormalizeStreamTargetBitrateMbps(targetBitrateMbps):0.#} Mbps";

    public static string FormatTransportRecordingConfigLabel(string? format, double targetBitrateMbps) =>
        $"{NormalizeRecordingFormat(format).ToUpperInvariant()} {NormalizeOutputTargetBitrateMbps(targetBitrateMbps):0.#} Mbps";

    private static string NormalizeVideoCodec(string? codec)
    {
        var normalized = codec?.Trim().ToLowerInvariant().Replace("hevc", "h265");
        return normalized is "h264" or "h265" or "av1" ? normalized : "h264";
    }

    private static string FormatVideoCodec(string? codec) => NormalizeVideoCodec(codec) switch
    {
        "h265" => "H.265",
        "av1" => "AV1",
        _ => "H.264"
    };

    private static string NormalizeStreamEncoderMode(string? value)
    {
        var normalized = value?.Trim().ToLowerInvariant();
        return normalized is "auto" or "nvenc" or "qsv" or "amf" or "cpu" ? normalized : "auto";
    }

    private static string NormalizeStreamRateControl(string? value) =>
        string.Equals(value?.Trim(), "vbr", StringComparison.OrdinalIgnoreCase) ? "vbr" : "cbr";

    private static string NormalizeStreamH264Profile(string? value)
    {
        var normalized = value?.Trim().ToLowerInvariant();
        return normalized is "auto" or "baseline" or "main" or "high" ? normalized : "high";
    }

    private static string FormatStreamEncoderMode(string? value) => NormalizeStreamEncoderMode(value) switch
    {
        "nvenc" => "NVENC",
        "qsv" => "QuickSync",
        "amf" => "AMF",
        "cpu" => "CPU",
        _ => "Auto encoder"
    };

    private static string NormalizeRecordingFormat(string? value)
    {
        var normalized = value?.Trim().ToLowerInvariant();
        return normalized is "mp4" or "mov" or "mkv" ? normalized : MediaCoreProductionSyncContext.DefaultRecordingTargets.Format;
    }

    private static string NormalizeRecordingQuality(string? value)
    {
        var normalized = value?.Trim().ToLowerInvariant();
        return normalized is "high" or "standard" or "archive" ? normalized : MediaCoreProductionSyncContext.DefaultRecordingTargets.Quality;
    }

    private static string FormatRecordingQuality(string? value) => NormalizeRecordingQuality(value) switch
    {
        "standard" => "Standard",
        "archive" => "Archive",
        _ => "High"
    };

    private static string NormalizeResolutionText(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return MediaCoreProductionSyncContext.DefaultCanvasOutputProfile.Resolution;
        }

        var normalized = value.Trim().ToLowerInvariant().Replace('×', 'x');
        var parts = normalized.Split('x', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return parts.Length == 2 &&
               int.TryParse(parts[0], out var width) &&
               int.TryParse(parts[1], out var height) &&
               width > 0 &&
               height > 0
            ? $"{width}x{height}"
            : MediaCoreProductionSyncContext.DefaultCanvasOutputProfile.Resolution;
    }

    private static double EstimateTargetBitrateMbps(int width, int height, int fps)
    {
        var pixels = width * height;
        var baseBitrate = pixels switch
        {
            >= 3840 * 2160 => 28.0,
            >= 2560 * 1440 => 16.0,
            >= 1920 * 1080 => 8.2,
            _ => 4.5
        };

        var fpsScale = fps >= 50 ? 1.0 : Math.Max(0.5, fps / 60.0);
        return Math.Round(baseBitrate * fpsScale, 1);
    }

    private string BuildSrtEncryptionSummary()
    {
        var keyLength = StudioStreamOutputValidation.ParseSrtKeyLength(StreamSrtKeyLength);
        return keyLength > 0 ? $"AES-{keyLength * 8}" : "no encryption";
    }

    private void MarkStreamingProfileCustom()
    {
        if (_applyingStreamingProfile ||
            string.Equals(StreamPlatformProfileId, StreamingProfileCatalog.CustomProfileId, StringComparison.Ordinal))
        {
            return;
        }

        _applyingStreamingProfile = true;
        try
        {
            StreamPlatformProfileId = StreamingProfileCatalog.CustomProfileId;
        }
        finally
        {
            _applyingStreamingProfile = false;
        }
    }

    private void OnOutputProfileChanged()
    {
        var canvasProfile = BuildRequestedOutputProfile("canvas", CanvasResolution, CanvasFps, "h264");
        ProgramResolutionLabel = TransportFormatting.ShortResolutionLabel(canvasProfile.Resolution, canvasProfile.Fps);
        OnPropertyChanged(nameof(CanvasProfileSummary));
        OnPropertyChanged(nameof(StreamRenderProfileSummary));
        OnPropertyChanged(nameof(StreamBitrateSummary));
        OnPropertyChanged(nameof(CompactStreamOutputLabel));
        OnPropertyChanged(nameof(RecordingBitrateSummary));
        OnPropertyChanged(nameof(CompactRecordingOutputLabel));
        OnPropertyChanged(nameof(RecordingRenderProfileSummary));
        OnPropertyChanged(nameof(StreamConfigurationSummary));
        OnPropertyChanged(nameof(RecordingOptionsSummary));
        RefreshTransportState();
        SaveProductionOutputPreferences();

        if (_bridge.Running)
        {
            _ = SyncOutputProfileChangeAsync();
        }
    }

    private async Task SyncOutputProfileChangeAsync()
    {
        if (Streaming && ValidateStreamDestinations() is { Length: > 0 } validationError)
        {
            LaunchLog.Write($"stream: profile change blocked invalid destination ({validationError})");
            var failureStatus = FormatStreamingFailureStatus("settings", new InvalidOperationException(validationError));
            RunOnUiThread(() =>
            {
                Streaming = false;
                RefreshOutputStatus();
                OutputStatus = $"{failureStatus} Streaming stopped.";
                OutputSessionStatus = OutputStatus;
            });

            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                var cleanupStatus = FormatStreamingFailureStatus("stop", ex);
                RunOnUiThread(() =>
                {
                    OutputStatus = $"Streaming settings cleanup failed: {cleanupStatus}";
                    OutputSessionStatus = OutputStatus;
                });
            }

            return;
        }

        await TrySyncMediaCoreAsync().ConfigureAwait(false);
    }

    private async void OnStreamOutputOptionChanged()
    {
        OnPropertyChanged(nameof(StreamDestinationSummary));
        OnPropertyChanged(nameof(StreamConfigurationSummary));
        OnPropertyChanged(nameof(StreamRtmpSummary));
        OnPropertyChanged(nameof(StreamNdiSummary));
        OnPropertyChanged(nameof(StreamSrtSummary));
        SaveProductionOutputPreferences();

        if (!Streaming || !_bridge.Running)
        {
            return;
        }

        if (ValidateStreamDestinations() is { Length: > 0 } validationError)
        {
            var failureStatus = FormatStreamingFailureStatus("settings", new InvalidOperationException(validationError));
            Streaming = false;
            RefreshOutputStatus();
            OutputStatus = $"{failureStatus} Streaming stopped.";
            OutputSessionStatus = OutputStatus;
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                var cleanupStatus = FormatStreamingFailureStatus("stop", ex);
                RunOnUiThread(() =>
                {
                    OutputStatus = $"Streaming settings cleanup failed: {cleanupStatus}";
                    OutputSessionStatus = OutputStatus;
                });
            }

            return;
        }

        try
        {
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            var failureStatus = FormatStreamingFailureStatus("settings", ex);
            RunOnUiThread(() =>
            {
                OutputStatus = failureStatus;
                OutputSessionStatus = OutputStatus;
            });
        }
    }

    private async void OnRecordingOutputOptionChanged()
    {
        OnPropertyChanged(nameof(RecordingOptionsSummary));
        OnPropertyChanged(nameof(CompactRecordingOutputLabel));
        RefreshTransportState();
        SaveProductionOutputPreferences();

        if (!Recording || !_bridge.Running)
        {
            return;
        }

        try
        {
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            RunOnUiThread(() =>
            {
                OutputStatus = $"Recording settings sync failed: {ex.Message}";
                OutputSessionStatus = OutputStatus;
            });
        }
    }

    private static string MapParticipantHealth(FeedHealth health) => health switch
    {
        FeedHealth.LowResolution => "low-resolution",
        FeedHealth.Recovering => "recovering",
        FeedHealth.VideoOff => "video-off",
        _ => "live"
    };

    private void RunOnUiThread(Action action)
    {
        if (_dispatcher.HasThreadAccess)
        {
            action();
            return;
        }

        _dispatcher.TryEnqueue(() => action());
    }

    private void OnBridgeStatusChanged(string status) =>
        RunOnUiThread(() => EngineStatus = status);

    private void OnBridgeProfileChanged(NativeMediaCoreProfile profile) =>
        RunOnUiThread(() =>
        {
            OnPropertyChanged(nameof(NativeCoreRuntimeStatus));
            OnPropertyChanged(nameof(NativeAudioRuntimeStatus));
        });

    private void OnBridgeHealthChanged(MediaCoreHealth health) =>
        RunOnUiThread(() => ApplyBridgeHealthChanged(health));

    private void ApplyBridgeHealthChanged(MediaCoreHealth health)
    {
        if (health.Stopped)
        {
            StopMediaCoreSession("Media core stopped");
            RefreshSurfaceBindings();
        }
        else if (health.Recovering)
        {
            EngineStatus = $"Media core recovering (restart {health.RestartCount})";
        }

        Settings.RefreshDiagnosticsReadout();
        OnPropertyChanged(nameof(NativeCoreRuntimeStatus));
        OnPropertyChanged(nameof(NativeAudioRuntimeStatus));
    }

    /// <summary>
    /// Supplies the configured stream and recording outputs for the support bundle.
    /// Endpoints and stream keys are redacted by <see cref="SupportBundleBuilder"/>.
    /// </summary>
    private IReadOnlyList<SupportBundleOutputDestination> BuildSupportBundleOutputDestinations()
    {
        var destinations = new List<SupportBundleOutputDestination>();
        var streamProfile = BuildRequestedOutputProfile(
            "stream",
            StreamRenderResolution,
            StreamRenderFps,
            StreamVideoCodec,
            NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps),
            NormalizeAudioBitrateKbps(StreamAudioBitrateKbps));
        var recordingProfile = BuildRequestedOutputProfile(
            "recording",
            RecordingRenderResolution,
            RecordingRenderFps,
            RecordingVideoCodec,
            NormalizeOutputTargetBitrateMbps(RecordingTargetBitrateMbps),
            NormalizeAudioBitrateKbps(RecordingAudioBitrateKbps));

        if (!string.IsNullOrWhiteSpace(StreamRtmpServerUrl) || !string.IsNullOrWhiteSpace(StreamRtmpStreamKey))
        {
            destinations.Add(new SupportBundleOutputDestination
            {
                Id = "rtmp",
                Name = "RTMP",
                Protocol = StreamRtmpProtocol,
                Enabled = !string.IsNullOrWhiteSpace(StreamRtmpServerUrl),
                Active = Streaming,
                Resolution = streamProfile.Resolution,
                Fps = streamProfile.Fps.ToString(CultureInfo.InvariantCulture),
                Codec = streamProfile.Codec,
                EncoderMode = NormalizeStreamEncoderMode(StreamEncoderMode),
                TargetBitrateMbps = streamProfile.TargetBitrateMbps,
                Endpoint = StreamRtmpServerUrl,
                StreamKey = StreamRtmpStreamKey
            });
        }

        if (StreamSrtEnabled || !string.IsNullOrWhiteSpace(StreamSrtHost))
        {
            destinations.Add(new SupportBundleOutputDestination
            {
                Id = "srt",
                Name = "SRT",
                Protocol = "srt",
                Enabled = StreamSrtEnabled,
                Active = Streaming && StreamSrtEnabled,
                Resolution = streamProfile.Resolution,
                Fps = streamProfile.Fps.ToString(CultureInfo.InvariantCulture),
                Codec = streamProfile.Codec,
                EncoderMode = NormalizeStreamEncoderMode(StreamEncoderMode),
                TargetBitrateMbps = streamProfile.TargetBitrateMbps,
                Endpoint = string.IsNullOrWhiteSpace(StreamSrtHost)
                    ? null
                    : $"srt://{StreamSrtHost}:{StreamSrtPort}",
                StreamKey = StreamSrtPassphrase
            });
        }

        if (StreamNdiEnabled)
        {
            destinations.Add(new SupportBundleOutputDestination
            {
                Id = "ndi",
                Name = "NDI",
                Protocol = "ndi",
                Enabled = StreamNdiEnabled,
                Active = Streaming && StreamNdiEnabled,
                Resolution = streamProfile.Resolution,
                Fps = streamProfile.Fps.ToString(CultureInfo.InvariantCulture),
                Codec = streamProfile.Codec,
                EncoderMode = NormalizeStreamEncoderMode(StreamEncoderMode),
                TargetBitrateMbps = streamProfile.TargetBitrateMbps,
                Endpoint = StreamNdiProgramName,
                StreamKey = null
            });
        }

        destinations.Add(new SupportBundleOutputDestination
        {
            Id = "recording",
            Name = "Recording",
            Protocol = "file",
            Enabled = true,
            Active = Recording,
            Resolution = recordingProfile.Resolution,
            Fps = recordingProfile.Fps.ToString(CultureInfo.InvariantCulture),
            Codec = recordingProfile.Codec,
            TargetBitrateMbps = recordingProfile.TargetBitrateMbps,
            Format = NormalizeRecordingFormat(RecordingFormat),
            Quality = NormalizeRecordingQuality(RecordingQuality),
            Endpoint = RecordingTargetFolder,
            StreamKey = null
        });

        return destinations;
    }

    private void OnSnapshotChanged(NativeMediaCoreStateSnapshot snapshot) =>
        RunOnUiThread(() => ApplySnapshotChanged(snapshot));

    private int _screenDiscoveryAttempts;
    private DateTime _screenDiscoveryLastAttempt = DateTime.MinValue;

    private void ApplySnapshotChanged(NativeMediaCoreStateSnapshot snapshot)
    {
        // P0 present-stutter-fix-spec: time each sub-op; log the breakdown only when a
        // snapshot apply is slow enough to stall the present (>10ms on the UI thread).
        var _swTotal = System.Diagnostics.Stopwatch.StartNew();
        var _swStep = System.Diagnostics.Stopwatch.StartNew();
        var _timings = new System.Text.StringBuilder();
        void Tm(string name)
        {
            _timings.Append(name).Append('=').Append(_swStep.Elapsed.TotalMilliseconds.ToString("F1")).Append("ms ");
            _swStep.Restart();
        }
        void LogIfSlow(string exit)
        {
            _swTotal.Stop();
            if (_swTotal.Elapsed.TotalMilliseconds > 10)
            {
                LaunchLog.Write($"perf: ApplySnapshot {_swTotal.Elapsed.TotalMilliseconds:F1}ms @{exit} :: {_timings}");
            }
        }

        // The compositor is always on, so surfaces always accept the latest program frame.
        _surfaces.OnMediaCoreSnapshot(snapshot);
        Tm("surfaces");

        // Screen sources come from the CORE enumeration; the startup device
        // refresh races core spawn (pipe not ready -> silent empty) and the
        // device WATCHER only fires on webcam plug events - so screens vanished
        // on fresh launches (owner-reported). RETRY on the snapshot heartbeat
        // until screens land (bounded; ~3s apart).
        if (_screenDiscoveryAttempts < 10 &&
            !CaptureDevices.Any(d => d.Id.StartsWith("screen:", StringComparison.Ordinal) || d.Id.StartsWith("window:", StringComparison.Ordinal)) &&
            (DateTime.UtcNow - _screenDiscoveryLastAttempt).TotalSeconds > 3)
        {
            _screenDiscoveryAttempts++;
            _screenDiscoveryLastAttempt = DateTime.UtcNow;
            _ = RefreshCaptureDevicesAsync();
        }

        // Always apply meeting/ZoomStatus/roster fields from the snapshot BEFORE any
        // early-return so meeting status keeps updating even when capture is unsubscribed.
        ApplyMeetingFieldsFromSnapshot(snapshot);
        Tm("meetingFields");
        var programResolutionLabel = ResolveProgramResolutionLabel(snapshot);
        ProgramResolutionLabel = programResolutionLabel;
        Transport.ApplySnapshot(
            snapshot,
            snapshot.Recording?.Active == true,
            LiveProductionSync.IsStreamingLive(snapshot),
            programResolutionLabel,
            MasterLimiterEnabled);
        Tm("transport");
        ApplyConfiguredOutputReadouts(snapshot);
        Tm("outputReadouts");
        RefreshAudioParticipantRows();
        Tm("audioRows");
        HydrateAudioRoutingMatrixFromSnapshot(snapshot);
        Tm("routingMatrix");
        RefreshVstPluginHostFromSnapshot(snapshot);
        Tm("vstHost");
        // C7b: the GR meter tracks the live per-snapshot value (cheap scalar
        // props — no collection churn).
        OnPropertyChanged(nameof(WorkspaceCompGrLevel));
        OnPropertyChanged(nameof(WorkspaceCompGrLabel));
        RefreshAudioReadoutBindings();
        Tm("audioReadouts");
        OnPropertyChanged(nameof(VirtualCameraStatusLabel));
        OnPropertyChanged(nameof(NativeLowerThirdStatus));
        OnPropertyChanged(nameof(NativeMediaPlaybackStatus));
        MaybeLogAudioTelemetry(snapshot);
        Tm("audioTelemetry");
        Settings.RefreshDiagnosticsReadout();
        Tm("diagnostics");

        if (!ZoomCaptureSubscribed)
        {
            LogIfSlow("notSubscribed");
            return;
        }

        var liveProductionContext = BuildLiveProductionContext();
        var autoStopStatus = LiveProductionSync.ResolveBreakoutRoomAutoStopStatus(
            snapshot,
            liveProductionContext.CurrentBreakoutRoomId);
        if (autoStopStatus is not null)
        {
            LogIfSlow("breakoutStop");
            StopEngineForBreakoutRoomChange(autoStopStatus);
            return;
        }

        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, liveProductionContext);
        Tm("mapPatch");
        ApplyLiveProductionPatch(patch);
        Tm("applyPatch");
        ApplyGraphicsAndCaptionStateFromSnapshot(snapshot);
        Tm("graphicsCaptions");
        LogIfSlow("full");
    }

    private LiveProductionSync.LiveProductionSyncContext BuildLiveProductionContext() =>
        new()
        {
            ActiveSceneId = ActiveSceneId,
            ActiveSceneLayout = ProgramScene.Layout,
            CurrentBreakoutRoomId = _currentRoomId,
            RecordingRequested = Recording,
            StreamingRequested = Streaming,
            Participants = RoomVideoParticipants
                .Select(participant => new LiveProductionSync.LiveProductionParticipantContext
                {
                    Id = participant.Id,
                    Name = participant.Name,
                    Title = participant.Title,
                    RoleLabel = participant.RoleLabel,
                    BreakoutRoomName = participant.BreakoutRoomName,
                    IsActiveSpeaker = participant.IsActiveSpeaker,
                    IsScreenSharing = participant.IsScreenSharing
                })
                .ToList()
        };

    // Unsubscribes the raw Zoom capture spine while leaving the media core / compositor
    // running. The program/preview surfaces fall back to a "capture paused / slate" state
    // (driven by the always-on compositor) rather than "waiting for compositor output".
    private void UnsubscribeZoomCapture(string status)
    {
        _bridge.ConfigureZoomSpineSync(null);
        _surfaces.SetZoomCaptureSubscribed(false);
        ZoomCaptureSubscribed = false;
        EngineStatus = status;
        CommandStatus = status;
        if (Settings.IsInMeeting && _bridge.LastSnapshot is { } snapshot)
        {
            ApplyMeetingFieldsFromSnapshot(snapshot);
        }
        else
        {
            ClearLiveProductionState();
        }

        Settings.RefreshSdkReadiness();
        RefreshSurfaceBindings();
        RefreshTransportState();
        ToggleEngineCommand.NotifyCanExecuteChanged();
        ToggleRecordingCommand.NotifyCanExecuteChanged();
    }

    private void StopMediaCoreSession(string status)
    {
        UnsubscribeZoomCapture(status);
        Recording = false;
        Streaming = false;
        _bridge.Stop();
        EngineStatus = status;
        Settings.RefreshSdkReadiness();
        RefreshTransportState();
    }

    private void StopEngineForBreakoutRoomChange(string status) => UnsubscribeZoomCapture(status);

    private void ApplyLiveProductionPatch(LiveProductionSync.StudioLiveProductionPatch patch)
    {
        // This sets x:Bound VM properties (OutputStatus, Recording, ZoomStatus, ...).
        // Callers reach here from SyncActiveSceneAsync's `await ....ConfigureAwait(false)`
        // continuation, i.e. a thread-pool thread — setting a bound property off the UI
        // thread fail-fasts WinUI (RPC_E_WRONG_THREAD -> 0xc000027b). Marshal to the UI
        // thread; this was the recurring ~30-60s crash.
        if (!_dispatcher.HasThreadAccess)
        {
            _dispatcher.TryEnqueue(() => ApplyLiveProductionPatch(patch));
            return;
        }

        // Suppress production-sync re-triggers while applying this sync's own response (see
        // TrySyncMediaCoreAsync) so the roster/multiview rebuilds below can't start a sync loop.
        _applyingProductionPatch = true;
        try
        {
        ApplyCaptionAndLowerThirdPatch(patch);

        if (patch.Recording is { } recording)
        {
            Recording = recording;
        }

        if (patch.Streaming is { } streaming)
        {
            Streaming = streaming;
        }

        if (patch.OutputStatus is { Length: > 0 } outputStatus)
        {
            OutputStatus = outputStatus;
        }

        if (patch.OutputSessionStatus is { Length: > 0 } outputSessionStatus)
        {
            OutputSessionStatus = outputSessionStatus;
        }

        if (patch.ZoomStatus is { Length: > 0 } zoomStatus)
        {
            ZoomStatus = zoomStatus;
        }

        if (patch.BreakoutRoomChangeHint is { Length: > 0 } breakoutHint)
        {
            CommandStatus = breakoutHint;
        }

        if (patch.MeetingStateLabel is { Length: > 0 } meetingStateLabel)
        {
            Settings.ApplyMeetingStateLabel(
                meetingStateLabel,
                patch.Participants?.Count ?? LiveParticipantCountFromPatch(patch));
        }

        if (patch.Participants is { Count: > 0 } participants)
        {
            ApplyLiveParticipants(participants);
        }
        else if (patch.Participants is { Count: 0 } && !Settings.IsInMeeting)
        {
            ClearLiveProductionParticipants();
        }
        }
        finally
        {
            _applyingProductionPatch = false;
        }
    }

    private static int LiveParticipantCountFromPatch(LiveProductionSync.StudioLiveProductionPatch patch) =>
        patch.Participants?.Count ?? 0;

    private string _liveStructureSignature = "";

    private void ApplyLiveParticipants(IReadOnlyList<LiveProductionSync.LiveProductionParticipantContext> participants)
    {
        var mapped = ParticipantMapper.ToParticipants(participants);

        // Only touch the structural UI (the room participant lists, multiview tiles,
        // Sources pickers) when the participant/device SET actually changes. The core
        // pushes a snapshot ~4x/second; reassigning RoomVideoParticipants to a NEW list
        // and firing its bindings EVERY snapshot re-realizes the bound item templates on
        // the UI thread (~5ms measured) and stalls the present -> operator-visible stutter
        // (present-stutter-fix-spec P1). Live video/audio/tally update via their own
        // binding paths, not these rebuilds, so when the set is unchanged there is nothing
        // to do here. The signature captures per-participant health/screen-share + the
        // device set, so a genuine change (join/leave/health/screen-share) still refreshes.
        // Signature must flip ONLY on genuinely structural changes: which participants
        // are present, whether each is video-OFF (VideoOff is filtered out of the video
        // list -> structural) or screen-sharing, and the device set. NOT on active-speaker
        // reordering (order-independent -> sort) and NOT on transient health flicker
        // (LowResolution/Recovering come and go every snapshot but do not add/remove a
        // tile). Including the raw enum + list order was rebuilding the whole roster/tile
        // UI ~4x/second under load = the operator stutter (present-stutter-fix-spec P1).
        var structureSignature =
            string.Join("|", mapped
                .Select(p => $"{p.Id}:{(p.Health == FeedHealth.VideoOff ? "off" : "on")}:{p.IsScreenSharing}")
                .OrderBy(static x => x, StringComparer.Ordinal)) +
            "#" + string.Join(",", CaptureDevices.Select(d => d.Id).OrderBy(static x => x, StringComparer.Ordinal));
        if (structureSignature == _liveStructureSignature)
        {
            return;  // set unchanged -> skip the churn (the common case, 4x/second)
        }
        _liveStructureSignature = structureSignature;

        RoomVideoParticipants = ParticipantMapper.VideoParticipantsInRoom(mapped, _currentRoomId);
        RoomParticipantsForInputs = ParticipantMapper.ParticipantsInRoom(mapped, _currentRoomId);
        CurrentRoomLabel = _currentRoomName;
        OnPropertyChanged(nameof(RoomVideoParticipants));
        OnPropertyChanged(nameof(CurrentRoomHeader));
        RefreshAudioParticipantRows();
        MultiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
        RefreshParticipantListItems();
        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
        OnPropertyChanged(nameof(CamerasOnCount));
        OnPropertyChanged(nameof(ScreenShareLabel));
        SchedulePreviewRoutingRefresh();
        RefreshProductionReadouts();
    }

    private void ApplyMeetingFieldsFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, BuildLiveProductionContext());
        var meetingState = patch.MeetingStateLabel;
        if (string.IsNullOrWhiteSpace(meetingState))
        {
            return;
        }

        if (meetingState.Equals("in_meeting", StringComparison.OrdinalIgnoreCase))
        {
            var _mf = System.Diagnostics.Stopwatch.StartNew();
            var _mfTotal = System.Diagnostics.Stopwatch.StartNew();
            var _mfb = new System.Text.StringBuilder();
            void MfT(string n) { _mfb.Append(n).Append('=').Append(_mf.Elapsed.TotalMilliseconds.ToString("F1")).Append("ms "); _mf.Restart(); }
            ZoomStatus = "Zoom Live";
            CurrentRoomLabel = _currentRoomName;
            ApplyCaptionAndLowerThirdPatch(patch);
            MfT("captionLT");
            ApplyCaptionTranscriptFromSnapshot(snapshot);
            MfT("transcript");
            var participants = LiveProductionSync.MapSnapshotParticipants(snapshot);
            MfT("mapParticipants");
            Settings.ApplyMeetingStateLabel(meetingState, participants?.Count ?? snapshot.Participants.Count);
            if (participants is { Count: > 0 })
            {
                ApplyLiveParticipants(participants);
                MfT("applyParticipants");
                SyncShowInputsFromMeeting(participants);
                MfT("syncShowInputs");
            }
            if (_mfTotal.Elapsed.TotalMilliseconds > 5)
            {
                LaunchLog.Write($"perf: meetingFields {_mfTotal.Elapsed.TotalMilliseconds:F1}ms :: {_mfb}");
            }
            return;
        }

        if (meetingState is "idle" or "leaving")
        {
            ZoomStatus = "Zoom Offline";
            Settings.ApplyMeetingStateLabel(meetingState, 0);
            ClearLiveProductionParticipants();
        }
    }

    private void ClearLiveProductionParticipants()
    {
        RoomVideoParticipants = [];
        CurrentRoomLabel = "No meeting";
        MultiviewTiles = [];
        OnPropertyChanged(nameof(RoomVideoParticipants));
        OnPropertyChanged(nameof(CurrentRoomHeader));
        RefreshParticipantListItems();
        RefreshAudioParticipantRows();
        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
        OnPropertyChanged(nameof(CamerasOnCount));
        OnPropertyChanged(nameof(ScreenShareLabel));
        SchedulePreviewRoutingRefresh();
        RefreshProductionReadouts();
    }

    private void ClearLiveProductionState()
    {
        ClearLiveProductionParticipants();
        CaptionText = string.Empty;
        CaptionSpeaker = string.Empty;
        LowerThirdName = string.Empty;
        LowerThirdTitle = string.Empty;
        LowerThirdOrg = string.Empty;
        ProgramLowerThirdKey = LowerThirdKeyState.Hidden(Overlays.LowerThirdPosition);
        _captionTranscriptPatches.Clear();
        CaptionTranscript = [];
        OnPropertyChanged(nameof(CaptionTranscript));
        OnPropertyChanged(nameof(CaptionQualitySummary));
        OnPropertyChanged(nameof(HasCaptionTranscript));
        OutputStatus = "Outputs idle";
        OutputSessionStatus = "Outputs idle";
        ZoomStatus = "Zoom Offline";
        RefreshTransportState();
    }

    private void InitializeGraphicsCatalog()
    {
        Graphics.Clear();
        foreach (var graphic in ProductionCatalog.DefaultGraphics)
        {
            Graphics.Add(new GraphicOverlay
            {
                Id = graphic.Id,
                Name = graphic.Name,
                Kind = graphic.Kind,
                Position = graphic.Position,
                Accent = graphic.Accent,
                Enabled = graphic.Enabled
            });
        }
    }

    private void ApplyCaptionAndLowerThirdPatch(LiveProductionSync.StudioLiveProductionPatch patch)
    {
        if (patch.CaptionText is { Length: > 0 } captionText)
        {
            CaptionText = captionText;
            Overlays.NotifyCaptionContentChanged();
        }

        if (patch.CaptionSpeaker is { Length: > 0 } captionSpeaker)
        {
            CaptionSpeaker = captionSpeaker;
        }

        if (patch.LowerThirdName is { Length: > 0 } lowerThirdName)
        {
            LowerThirdName = lowerThirdName;
        }

        if (patch.LowerThirdTitle is { Length: > 0 } lowerThirdTitle)
        {
            LowerThirdTitle = lowerThirdTitle;
        }

        if (patch.LowerThirdOrg is { Length: > 0 } lowerThirdOrg)
        {
            LowerThirdOrg = lowerThirdOrg;
        }
    }

    private void ApplyGraphicsAndCaptionStateFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        ApplyOverlayStateFromSnapshot(snapshot);
        ApplyProductionReadoutsFromSnapshot(snapshot);
        ApplyCaptionTranscriptFromSnapshot(snapshot);
    }

    private void ApplyProductionReadoutsFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var colorGradePatch = ProductionReadoutSync.ResolveColorGradePatch(snapshot, ZoomCaptureSubscribed);
        if (colorGradePatch is not null)
        {
            ColorGrade = new ColorGrade
            {
                Lut = colorGradePatch.Lut,
                Exposure = colorGradePatch.Exposure,
                Contrast = colorGradePatch.Contrast,
                Saturation = colorGradePatch.Saturation,
                Temperature = colorGradePatch.Temperature
            };
        }

        var brandKitPatch = ProductionReadoutSync.ResolveBrandKitPatch(snapshot, ZoomCaptureSubscribed);
        if (brandKitPatch is not null)
        {
            BrandKit = new BrandKit
            {
                Name = brandKitPatch.Name,
                LogoText = brandKitPatch.LogoText,
                LogoAssetId = BrandKit.LogoAssetId,
                LogoAssetName = BrandKit.LogoAssetName,
                LogoAssetPath = BrandKit.LogoAssetPath,
                BrandColor = brandKitPatch.BrandColor,
                AccentColor = brandKitPatch.AccentColor,
                BackgroundColor = brandKitPatch.BackgroundColor,
                FontFamily = brandKitPatch.FontFamily,
                LowerThirdStyle = brandKitPatch.LowerThirdStyle,
                CaptionStyle = BrandKit.CaptionStyle,
                DefaultOverlayBehavior = BrandKit.DefaultOverlayBehavior
            };
            Overlays.NotifyBrandKitChanged();
        }
    }

    private void ApplyOverlayStateFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var enabledFlags = GraphicsOverlaySync.ResolveOverlayEnabledFlags(
            snapshot,
            Graphics.Select(graphic => graphic.Id),
            ZoomCaptureSubscribed);
        if (enabledFlags is null)
        {
            return;
        }

        foreach (var graphic in Graphics)
        {
            if (enabledFlags.TryGetValue(graphic.Id, out var enabled))
            {
                graphic.Enabled = enabled;
            }
        }

        OnPropertyChanged(nameof(EnabledGraphics));
    }

    private void ApplyCaptionTranscriptFromSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        var speakerRoles = RoomVideoParticipants.ToDictionary(
            participant => participant.Name,
            participant => participant.RoleLabel,
            StringComparer.Ordinal);
        var nextTranscript = GraphicsOverlaySync.AppendCaptionTranscriptFromSnapshot(
            snapshot,
            _captionTranscriptPatches,
            speakerRoles);
        if (nextTranscript.Count == _captionTranscriptPatches.Count)
        {
            return;
        }

        _captionTranscriptPatches.Clear();
        _captionTranscriptPatches.AddRange(nextTranscript);
        CaptionTranscript = _captionTranscriptPatches
            .Select(entry => new CaptionTranscriptEntry
            {
                Id = entry.Id,
                SpeakerName = entry.SpeakerName,
                Role = entry.Role,
                Text = entry.Text,
                Confidence = entry.Confidence
            })
            .ToList();
        OnPropertyChanged(nameof(CaptionTranscript));
        OnPropertyChanged(nameof(CaptionQualitySummary));
        OnPropertyChanged(nameof(HasCaptionTranscript));
    }

    private void SyncShowInputsFromMeeting(
        IReadOnlyList<LiveProductionSync.LiveProductionParticipantContext> participants)
    {
        // Free slots whose participant left, and (when auto-assign is on) fill FREE slots
        // with newly-joined participants without disturbing operator/capture assignments.
        ShowInputRosterService.SyncZoomParticipantSlots(
            ShowInputs,
            participants.Select(participant => participant.Id).ToList(),
            AutomationAutoAssignInputsEnabled);

        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
    }

    // Re-run the roster→slot auto-assign from the CURRENT room roster (used when the operator
    // flips the auto-assign toggle). No-op stale-removal when there is no meeting.
    // MUST use RoomParticipantsForInputs (all in-room, video on OR off) — the SAME set the
    // meeting-sync path passes. RoomVideoParticipants filters out camera-off participants, so
    // using it here would make the helper's stale-removal pass free a camera-off participant's
    // assigned slot the moment the operator flips the toggle.
    private void ReapplyShowInputAutoAssign()
    {
        ShowInputRosterService.SyncZoomParticipantSlots(
            ShowInputs,
            RoomParticipantsForInputs
                .Select(participant => participant.Id)
                .Where(id => !string.IsNullOrWhiteSpace(id))
                .ToList(),
            AutomationAutoAssignInputsEnabled);

        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
    }

    private void RefreshParticipantListItems()
    {
        ParticipantListItems = RoomVideoParticipants
            .Select(participant => StudioHighlightBrushes.ForParticipant(
                participant,
                SelectedParticipantId,
                SelectParticipantCommand))
            .ToList();
        OnPropertyChanged(nameof(ParticipantListItems));
    }

    private void RefreshAudioParticipantRows()
    {
        RefreshAudioMixChannels();
        var participantsById = RoomVideoParticipants
            .Where(participant => !string.IsNullOrWhiteSpace(participant.Id))
            .GroupBy(participant => participant.Id, StringComparer.Ordinal)
            .ToDictionary(group => group.Key, group => group.Last(), StringComparer.Ordinal);
        var rows = _audioMixChannels
            .OrderBy(channel => ResolveAudioRowSortKey(channel.ParticipantId, participantsById))
            .Select(mix =>
            {
                participantsById.TryGetValue(mix.ParticipantId, out var participant);
                participant ??= new Participant
                {
                    Id = mix.ParticipantId,
                    Name = ResolveAudioSourceDisplayName(mix.ParticipantId),
                    Role = ParticipantRole.Guest,
                    BreakoutRoomName = "Native PCM",
                    Health = mix.OutputLevel > 0 ? FeedHealth.Live : FeedHealth.Recovering,
                    AudioLevel = mix.OutputLevel,
                    IsMuted = mix.Muted
                };

                return new AudioParticipantRow
                {
                    Id = participant.Id,
                    Name = participant.Name,
                    Subtitle = $"{participant.RoleLabel} · {participant.BreakoutRoomName} · {participant.HealthLabel}",
                    OutputLevel = Math.Clamp(mix.OutputLevel, 0, 100),
                    ManualGainDb = NormalizeMixerGain(mix.ManualGainDb),
                    Pan = NormalizeMixerPan(mix.Pan),
                    Lufs = NormalizeMeterDb(mix.Lufs),
                    TruePeakDb = NormalizeMeterDb(mix.TruePeakDb),
                    Muted = mix.Muted,
                    IsSolo = mix.Solo,
                    GainLabel = $"{(NormalizeMixerGain(mix.ManualGainDb) > 0 ? "+" : "")}{NormalizeMixerGain(mix.ManualGainDb):0.0} dB",
                    PanLabel = Math.Abs(NormalizeMixerPan(mix.Pan)) < 0.01
                        ? "C"
                        : NormalizeMixerPan(mix.Pan) < 0
                            ? $"L {Math.Abs(NormalizeMixerPan(mix.Pan)):0.00}"
                            : $"R {NormalizeMixerPan(mix.Pan):0.00}",
                    LufsLabel = FormatMixerLufsLabel(mix.Lufs),
                    TruePeakLabel = $"{NormalizeMeterDb(mix.TruePeakDb):0.0} dBTP",
                    BusLabel = ResolvePrimaryAudioBusLabel(mix.ParticipantId),
                    InsertLabel = mix.PluginInserts.Count == 0
                        ? "No inserts"
                        : string.Join(" + ", mix.PluginInserts),
                    MuteButtonLabel = mix.Muted ? "Unmute" : "Mute",
                    MuteStateLabel = mix.Muted ? "Muted" : "Live",
                    IsSelected = mix.ParticipantId == SelectedParticipantId
                };
            })
            .ToList();
        if (AudioParticipantRows.Count == rows.Count &&
            AudioParticipantRows.Select(row => row.Id).SequenceEqual(rows.Select(row => row.Id), StringComparer.Ordinal))
        {
            for (var index = 0; index < rows.Count; index++)
            {
                UpdateAudioParticipantRow(AudioParticipantRows[index], rows[index]);
            }
        }
        else
        {
            AudioParticipantRows.Clear();
            foreach (var row in rows)
            {
                AudioParticipantRows.Add(row);
            }
        }

        OnPropertyChanged(nameof(AudioMixerEmptyStateVisibility));
        OnPropertyChanged(nameof(AudioMixerRowsVisibility));
        RefreshAudioProcessingTargets();
    }

    private void RefreshAudioParticipantRow(string participantId)
    {
        var mix = _audioMixChannels.FirstOrDefault(channel =>
            string.Equals(channel.ParticipantId, participantId, StringComparison.Ordinal));
        var target = AudioParticipantRows.FirstOrDefault(row =>
            string.Equals(row.Id, participantId, StringComparison.Ordinal));
        if (mix is null || target is null)
        {
            return;
        }

        var participant = RoomVideoParticipants.FirstOrDefault(item =>
            string.Equals(item.Id, mix.ParticipantId, StringComparison.Ordinal)) ?? new Participant
            {
                Id = mix.ParticipantId,
                Name = ResolveAudioSourceDisplayName(mix.ParticipantId),
                Role = ParticipantRole.Guest,
                BreakoutRoomName = "Native PCM",
                Health = mix.OutputLevel > 0 ? FeedHealth.Live : FeedHealth.Recovering,
                AudioLevel = mix.OutputLevel,
                IsMuted = mix.Muted
            };
        var gain = NormalizeMixerGain(mix.ManualGainDb);
        var pan = NormalizeMixerPan(mix.Pan);
        var lufs = NormalizeMeterDb(mix.Lufs);
        var truePeak = NormalizeMeterDb(mix.TruePeakDb);

        UpdateAudioParticipantRow(target, new AudioParticipantRow
        {
            Id = participant.Id,
            Name = participant.Name,
            Subtitle = $"{participant.RoleLabel} · {participant.BreakoutRoomName} · {participant.HealthLabel}",
            OutputLevel = Math.Clamp(mix.OutputLevel, 0, 100),
            ManualGainDb = gain,
            Pan = pan,
            Lufs = lufs,
            TruePeakDb = truePeak,
            Muted = mix.Muted,
            IsSolo = mix.Solo,
            GainLabel = $"{(gain > 0 ? "+" : "")}{gain:0.0} dB",
            PanLabel = Math.Abs(pan) < 0.01
                ? "C"
                : pan < 0
                    ? $"L {Math.Abs(pan):0.00}"
                    : $"R {pan:0.00}",
            LufsLabel = FormatMixerLufsLabel(lufs),
            TruePeakLabel = $"{truePeak:0.0} dBTP",
            BusLabel = ResolvePrimaryAudioBusLabel(mix.ParticipantId),
            InsertLabel = mix.PluginInserts.Count == 0
                ? "No inserts"
                : string.Join(" + ", mix.PluginInserts),
            MuteButtonLabel = mix.Muted ? "Unmute" : "Mute",
            MuteStateLabel = mix.Muted ? "Muted" : "Live",
            IsSelected = mix.ParticipantId == SelectedParticipantId
        });
    }

    private void RefreshAudioParticipantSelectionRows()
    {
        foreach (var row in AudioParticipantRows)
        {
            row.IsSelected = string.Equals(row.Id, SelectedParticipantId, StringComparison.Ordinal);
        }
    }

    private static void UpdateAudioParticipantRow(AudioParticipantRow target, AudioParticipantRow source)
    {
        target.Name = source.Name;
        target.Subtitle = source.Subtitle;
        target.OutputLevel = source.OutputLevel;
        target.ManualGainDb = source.ManualGainDb;
        target.Pan = source.Pan;
        target.Lufs = source.Lufs;
        target.TruePeakDb = source.TruePeakDb;
        target.Muted = source.Muted;
        target.IsSolo = source.IsSolo;
        target.GainLabel = source.GainLabel;
        target.PanLabel = source.PanLabel;
        target.LufsLabel = source.LufsLabel;
        target.TruePeakLabel = source.TruePeakLabel;
        target.BusLabel = source.BusLabel;
        target.InsertLabel = source.InsertLabel;
        target.MuteButtonLabel = source.MuteButtonLabel;
        target.MuteStateLabel = source.MuteStateLabel;
        target.IsSelected = source.IsSelected;
    }

    public static string FormatMixerLufsLabel(double value) =>
        $"{NormalizeMeterDb(value):0.0} LUFS";

    private void RefreshAudioProcessingTargets()
    {
        var targets = new List<AudioProcessingTargetOption>();

        foreach (var participant in RoomVideoParticipants)
        {
            var targetId = FormatChannelProcessingTargetId(participant.Id);
            var mix = _audioMixChannels.FirstOrDefault(channel =>
                string.Equals(channel.ParticipantId, participant.Id, StringComparison.Ordinal));
            var inserts = mix?.PluginInserts ?? [];
            targets.Add(new AudioProcessingTargetOption
            {
                Id = targetId,
                Label = participant.Name,
                Kind = "Channel",
                Detail = $"{participant.RoleLabel} channel - {ResolvePrimaryAudioBusLabel(participant.Id)}",
                InsertLabel = FormatInsertLabel(inserts)
            });
        }

        foreach (var mix in _audioMixChannels.Where(channel =>
                     RoomVideoParticipants.All(participant =>
                         !string.Equals(participant.Id, channel.ParticipantId, StringComparison.Ordinal))))
        {
            var targetId = FormatChannelProcessingTargetId(mix.ParticipantId);
            targets.Add(new AudioProcessingTargetOption
            {
                Id = targetId,
                Label = ResolveAudioSourceDisplayName(mix.ParticipantId),
                Kind = "Channel",
                Detail = $"Native PCM channel - {ResolvePrimaryAudioBusLabel(mix.ParticipantId)}",
                InsertLabel = FormatInsertLabel(mix.PluginInserts)
            });
        }

        // The master path is a first-class processing target even before the
        // routing matrix has hydrated its bus headers. Setup should never open
        // on an empty target while presenting master-bus controls.
        var masterTargetId = FormatBusProcessingTargetId("master");
        targets.Add(new AudioProcessingTargetOption
        {
            Id = masterTargetId,
            Label = ResolveAudioProcessingTargetName(masterTargetId),
            Kind = ResolveBusProcessingKind("master"),
            Detail = ResolveBusProcessingDetail("master"),
            InsertLabel = FormatInsertLabel(ResolveAudioProcessingInserts(masterTargetId))
        });

        foreach (var bus in AudioRoutingMatrix.BusHeaders.Where(bus =>
                     !string.Equals(bus.Id, "master", StringComparison.OrdinalIgnoreCase)))
        {
            var targetId = FormatBusProcessingTargetId(bus.Id);
            targets.Add(new AudioProcessingTargetOption
            {
                Id = targetId,
                Label = bus.Label,
                Kind = ResolveBusProcessingKind(bus.Id),
                Detail = ResolveBusProcessingDetail(bus.Id),
                InsertLabel = FormatInsertLabel(ResolveAudioProcessingInserts(targetId))
            });
        }

        var targetsSignature = string.Join(
            "\u001f",
            targets.Select(target => string.Join(
                "\u001e",
                target.Id,
                target.Label,
                target.Kind,
                target.Detail,
                target.InsertLabel)));
        var targetsChanged = !string.Equals(
            targetsSignature,
            _audioProcessingTargetsSignature,
            StringComparison.Ordinal);
        if (targetsChanged)
        {
            // Meter/audio snapshots arrive while the operator may be using this
            // ComboBox. Keep the existing ItemsSource instance unless the target
            // structure or insert labels actually changed; replacing it for every
            // PCM update can reopen the popup or steal focus while someone speaks.
            AudioProcessingTargetOptions = targets;
            _audioProcessingTargetsSignature = targetsSignature;
            OnPropertyChanged(nameof(AudioProcessingTargetOptions));
        }

        if (AudioProcessingTargetOptions.Count > 0 &&
            AudioProcessingTargetOptions.All(target =>
                !string.Equals(target.Id, SelectedAudioProcessingTargetId, StringComparison.Ordinal)))
        {
            SelectedAudioProcessingTargetId = AudioProcessingTargetOptions.First().Id;
        }

        if (targetsChanged)
        {
            OnPropertyChanged(nameof(SelectedAudioProcessingTarget));
            OnPropertyChanged(nameof(SelectedAudioProcessingTargetLabel));
            OnPropertyChanged(nameof(SelectedAudioProcessingTargetKindLabel));
            OnPropertyChanged(nameof(SelectedAudioProcessingTargetDetail));
            OnPropertyChanged(nameof(SelectedAudioProcessingInsertLabel));
        }
        OnPropertyChanged(nameof(ProcessingBridgeStatusLabel));
        RefreshSelectedAudioProcessingSlots();
    }

    private static string FormatInsertLabel(IReadOnlyList<string> inserts) =>
        inserts.Count == 0 ? "No inserts" : string.Join(" + ", inserts);

    private static string ResolveBusProcessingKind(string busId)
    {
        if (busId.Equals("master", StringComparison.OrdinalIgnoreCase))
        {
            return "Master bus";
        }

        if (busId.StartsWith("aux-", StringComparison.OrdinalIgnoreCase))
        {
            return "Aux bus";
        }

        if (busId.StartsWith("iso-", StringComparison.OrdinalIgnoreCase))
        {
            return "ISO bus";
        }

        if (busId.StartsWith("bus-", StringComparison.OrdinalIgnoreCase))
        {
            return "Custom bus";
        }

        return "Bus";
    }

    private static string ResolveBusProcessingDetail(string busId) => busId switch
    {
        "master" => "Final program mix processing before output.",
        "stream" => "Streaming output bus processing.",
        "mon" => "Control-room monitor bus processing.",
        "pgm-l" => "Left program bus processing.",
        "pgm-r" => "Right program bus processing.",
        _ when busId.StartsWith("aux-", StringComparison.OrdinalIgnoreCase) => "Aux send processing.",
        _ when busId.StartsWith("iso-", StringComparison.OrdinalIgnoreCase) => "Isolated recording bus processing.",
        _ => "Custom bus processing."
    };

    private string ResolvePrimaryAudioBusLabel(string participantId)
    {
        var routedBus = AudioRoutingMatrix.Rows
            .Where(row => string.Equals(ResolveAudioRoutingMatrixSourceId(row.SourceId), participantId, StringComparison.Ordinal))
            .SelectMany(row => row.Cells)
            .Where(cell => cell.IsRouted)
            .Select(cell => cell.Bus.Label)
            .FirstOrDefault();

        return routedBus ?? "MASTER";
    }

    private string ResolveAudioRoutingMatrixSourceId(string sourceId)
    {
        if (!TryParseInputSourceId(sourceId, out var slotNumber) ||
            ShowInputs.FirstOrDefault(slot => slot.SlotNumber == slotNumber) is not { } input)
        {
            return sourceId;
        }

        if (input.Kind == ShowInputKind.ZoomParticipant && input.ParticipantId is { Length: > 0 } participantId)
        {
            return participantId;
        }

        if (input.CaptureDeviceId is not { Length: > 0 } captureDeviceId)
        {
            return sourceId;
        }

        return input.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest
            ? $"capture:{captureDeviceId}"
            : captureDeviceId;
    }

    public static string ResolveLocalAudioRoutingSourceLabel(string? selectedLocalAudioCaptureDeviceName)
    {
        var deviceName = string.IsNullOrWhiteSpace(selectedLocalAudioCaptureDeviceName)
            ? "No local audio input selected"
            : selectedLocalAudioCaptureDeviceName.Trim();
        return $"Local machine audio - {deviceName}";
    }

    private string ResolveAudioSourceDisplayName(string sourceId)
    {
        if (string.Equals(sourceId, "local-machine-audio", StringComparison.OrdinalIgnoreCase))
        {
            return ResolveLocalAudioRoutingSourceLabel(SelectedLocalAudioCaptureDeviceName);
        }

        if (sourceId.StartsWith("capture:", StringComparison.OrdinalIgnoreCase))
        {
            var captureId = sourceId["capture:".Length..];
            return CaptureDevices.FirstOrDefault(device => device.Id == captureId)?.Name ?? $"Capture {captureId}";
        }

        return sourceId switch
        {
            "zoom-mix" => "Zoom program mix",
            "media" => "Media playback",
            _ => sourceId
        };
    }

    private int ResolveAudioRowSortKey(
        string participantId,
        IReadOnlyDictionary<string, Participant> participantsById)
    {
        var index = 0;
        foreach (var participant in RoomVideoParticipants)
        {
            if (string.Equals(participant.Id, participantId, StringComparison.Ordinal))
            {
                return index;
            }

            index++;
        }

        return 10_000 + Math.Abs(StringComparer.OrdinalIgnoreCase.GetHashCode(participantId) % 1_000);
    }

    private int _surfaceRefreshPending;
    private long _lastRsbTickMs;
    private bool _rsbThrottleScheduled;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _rsbThrottleTimer;
    private const long RsbMinIntervalMs = 80;  // cap surface-binding refresh to ~12.5/s

    private void OnSurfacesChanged()
    {
        // Surfaces change up to ~100x/s (per-source pixel frames). The old 1-pending
        // coalesce did NOT cap the RATE — it re-ran RefreshSurfaceBindings as fast as the
        // UI thread drained (~98/s), and each run REBUILDS MultiviewTiles (wholesale
        // collection replace). That frame-rate rebuild + dispatcher flood is the
        // multi-participant fail-fast (CoreMessagingXP 0xc000027b — confirmed reproduced).
        // GPU surfaces self-refresh at 60fps via their own present loop, so bindings only
        // need to catch STRUCTURAL changes within ~80ms. Throttle to ~12.5/s.
        if (System.Threading.Interlocked.Exchange(ref _surfaceRefreshPending, 1) == 1)
        {
            return;
        }

        RunOnUiThread(ApplyThrottledSurfaceRefresh);
    }

    private void ApplyThrottledSurfaceRefresh()
    {
        System.Threading.Interlocked.Exchange(ref _surfaceRefreshPending, 0);
        var now = Environment.TickCount64;
        var since = now - _lastRsbTickMs;
        if (since >= RsbMinIntervalMs)
        {
            _lastRsbTickMs = now;
            RefreshSurfaceBindings();
            return;
        }
        // Too soon — schedule a single trailing refresh so the latest state still lands.
        if (_rsbThrottleScheduled)
        {
            return;
        }
        _rsbThrottleScheduled = true;
        if (_rsbThrottleTimer is null)
        {
            _rsbThrottleTimer = _dispatcher.CreateTimer();
            _rsbThrottleTimer.IsRepeating = false;
            _rsbThrottleTimer.Tick += (_, _) =>
            {
                _rsbThrottleScheduled = false;
                _lastRsbTickMs = Environment.TickCount64;
                RefreshSurfaceBindings();
            };
        }
        _rsbThrottleTimer.Interval = TimeSpan.FromMilliseconds(RsbMinIntervalMs - since);
        _rsbThrottleTimer.Start();
    }

    private long _rsbCount;
    private long _rsbTotalMs;
    private long _rsbWindowStartMs;
    private void RefreshSurfaceBindings()
    {
        // MUST run on the UI thread: it assigns ProgramSurface/PreviewSurface/MultiviewTiles
        // (all x:Bound to VideoSurfaceHosts). Off-thread = native fail-fast (CoreMessagingXP
        // 0xc000027b). Marshal defensively; runs inline when already on the UI thread.
        if (!_dispatcher.HasThreadAccess)
        {
            _dispatcher.TryEnqueue(RefreshSurfaceBindings);
            return;
        }

        var sw = System.Diagnostics.Stopwatch.StartNew();
        ProgramSurface = _surfaces.ProgramSurface;
        // The MULTIVIEW monitor is now ONE core-composited GPU shared texture (single swap chain),
        // mirrored from the coordinator on structural change only — NOT a per-frame rebuild of N
        // bound tiles (the retired CPU/per-tile-swap-chain churn vector). MultiviewTiles is still
        // built here because the PREVIEW bus resolves its primary source's live surface from it
        // (program/preview stay), but it no longer drives the multiview host.
        MultiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
        MultiviewSurface = _surfaces.MultiviewSurface;
        MultiviewTileRects = _surfaces.MultiviewTileRects;
        // Set the preview bus AFTER MultiviewTiles is rebuilt so the previewed source
        // resolves against the freshly-built live surfaces. RefreshSurfaceBindings is the
        // single UI-thread path that mutates VideoSurfaceHost-bound properties (Program,
        // Preview, Multiview), so this is safe (no off-thread VideoSurfaceHost writes).
        // Prefer the core-composited PREVIEW bus (a real multi-layer preview) when present;
        // fall back to the single-source resolution only when no preview composite is set.
        PreviewSurface = _surfaces.HasPreviewComposite
            ? _surfaces.PreviewCompositeSurface
            : ResolvePreviewPrimarySurface();
        RefreshOpenColorGradeEditorPreviews();
        SchedulePreviewRoutingRefresh();
        sw.Stop();
        // DIAGNOSTIC: how much UI-thread time the per-frame binding rebuild consumes.
        // UIbusy% near 100 means the rebuild saturates the UI thread -> frames lag.
        _rsbTotalMs += sw.ElapsedMilliseconds;
        _rsbCount++;
        var now = Environment.TickCount64;
        if (_rsbWindowStartMs == 0) { _rsbWindowStartMs = now; }
        if (_rsbCount >= 60)
        {
            var span = now - _rsbWindowStartMs;
            LaunchLog.Write($"perf: RefreshSurfaceBindings {_rsbCount} calls/{span}ms => {(double)_rsbTotalMs / _rsbCount:F1}ms/call, {(span > 0 ? _rsbCount * 1000.0 / span : 0):F0}/s, UIbusy={(span > 0 ? 100.0 * _rsbTotalMs / span : 0):F0}%");
            _rsbCount = 0;
            _rsbTotalMs = 0;
            _rsbWindowStartMs = now;
        }
    }

    /// <summary>
    /// Resolves the surface the PREVIEW monitor presents: the queued scene's PRIMARY
    /// source, ATEM preview-bus style. PreviewSceneTiles[0] identifies that source (already
    /// resolved from the queued scene's routes); we pull its LIVE surface from the
    /// freshly-built MultiviewTiles so the preview updates every frame instead of from the
    /// scene-publish snapshot. Falls back to the snapshot tile, then to the coordinator's
    /// preview surface (selected-participant / capture-paused slate). Must run on the UI
    /// thread (it feeds a VideoSurfaceHost-bound property) — it is only called from
    /// RefreshSurfaceBindings, which is always marshalled to the UI thread.
    /// </summary>
    private string _lastPreviewResolveSig = string.Empty;
    private VideoSurfaceState ResolvePreviewPrimarySurface()
    {
        var primary = PreviewSceneTiles is { Count: > 0 } tiles ? tiles[0] : null;
        if (primary is not null)
        {
            string branch;
            VideoSurfaceState? liveSurface =
                MultiviewTiles.FirstOrDefault(t => t.Participant.Id == primary.Participant.Id)?.Surface;
            branch = liveSurface is not null ? "multiview-tile" : "none";

            // Capture-device primaries are not part of the participant multiview roster;
            // pull their live surface from the coordinator's capture surface map.
            if (liveSurface is null &&
                primary.Participant.Id.StartsWith("capture:", StringComparison.Ordinal))
            {
                var deviceId = primary.Participant.Id["capture:".Length..];
                if (_surfaces.CaptureDeviceSurfaces.TryGetValue(deviceId, out var captureSurface))
                {
                    liveSurface = captureSurface;
                    branch = "capture-map";
                }
            }

            if (liveSurface is null && primary.Surface is not null)
            {
                liveSurface = primary.Surface;
                branch = "primary.Surface";
            }
            if (liveSurface is not null)
            {
                var resolved = liveSurface with
                {
                    SurfaceKey = "preview",
                    Kind = VideoSurfaceKind.Preview,
                    Title = "Preview"
                };
                // DIAGNOSTIC (preview-freeze): log only when branch / handle / primary id changes.
                var h = resolved.PendingSharedHandle;
                var hv = h is { IsValid: true } hh ? hh.NtHandle : 0UL;
                var sig = $"{branch}|{primary.Participant.Id}|0x{hv:X}";
                if (sig != _lastPreviewResolveSig)
                {
                    _lastPreviewResolveSig = sig;
                    LaunchLog.Write($"preview-resolve: branch={branch} primary={primary.Participant.Id} handle=0x{hv:X} valid={(hv != 0)}");
                }
                return resolved;
            }
        }

        var fallback = _surfaces.PreviewSurface;
        var fh = fallback.PendingSharedHandle;
        var fhv = fh is { IsValid: true } fhh ? fhh.NtHandle : 0UL;
        var fsig = $"fallback|{primary?.Participant.Id ?? "<none>"}|0x{fhv:X}";
        if (fsig != _lastPreviewResolveSig)
        {
            _lastPreviewResolveSig = fsig;
            LaunchLog.Write($"preview-resolve: branch=fallback(_surfaces.PreviewSurface) primary={primary?.Participant.Id ?? "<none>"} handle=0x{fhv:X} valid={(fhv != 0)}");
        }
        return fallback;
    }

    private void ScheduleMultiviewGridRefresh()
    {
        if (_multiviewGridRefreshScheduled)
        {
            return;
        }

        _multiviewGridRefreshScheduled = true;
        _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _multiviewGridRefreshScheduled = false;
            RefreshMultiviewGridTiles();
        });
    }

    private static IProductionOutputPreferencesStore CreateProductionOutputPreferencesStore()
    {
        try
        {
            var folder = Windows.Storage.ApplicationData.Current.LocalFolder.Path;
            return new FileProductionOutputPreferencesStore(folder);
        }
        catch (Exception)
        {
            try
            {
                var folder = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "CoreVideoPro");
                return new FileProductionOutputPreferencesStore(folder);
            }
            catch (Exception)
            {
                return new InMemoryProductionOutputPreferencesStore();
            }
        }
    }

    private void LoadProductionOutputPreferences()
    {
        try
        {
            var preferences = _outputPreferencesStore.Load();
            if (preferences is not null)
            {
                ApplyProductionOutputPreferences(preferences);
            }
        }
        catch (Exception)
        {
            // Output settings are best-effort; defaults must still allow startup.
        }
        finally
        {
            _outputPreferencesLoaded = true;
        }
    }

    // The operator-facing display name for a canonical source id, defaulting to the
    // derived Zoom/UVC/asset name when there is no override.
    public string ResolveSourceDisplayName(string? sourceId, string derivedName) =>
        ShowInputRosterService.ResolveDisplayName(_sourceDisplayNames, sourceId, derivedName);

    // Set (or, when blank, clear/reset) the operator override for a source, then persist
    // and propagate it to the auto lower-thirds and multiview labels.
    public void SetSourceDisplayName(string? sourceId, string? name)
    {
        if (string.IsNullOrWhiteSpace(sourceId))
        {
            return;
        }

        var trimmed = name?.Trim();
        bool changed;
        if (string.IsNullOrWhiteSpace(trimmed))
        {
            changed = _sourceDisplayNames.Remove(sourceId);  // reset to the derived name
        }
        else if (!_sourceDisplayNames.TryGetValue(sourceId, out var existing) ||
                 !string.Equals(existing, trimmed, StringComparison.Ordinal))
        {
            _sourceDisplayNames[sourceId] = trimmed;
            changed = true;
        }
        else
        {
            changed = false;
        }

        if (!changed)
        {
            return;
        }

        SaveProductionOutputPreferences();
        // Resend the multiview layout (its labels are override-aware) and recompute the
        // program lower-third so a rename shows immediately.
        ScheduleShowInputRefresh();
        RefreshProgramLowerThirdKeyPosition();
    }

    private void SaveProductionOutputPreferences()
    {
        if (!_outputPreferencesLoaded)
        {
            return;
        }

        try
        {
            _outputPreferencesStore.Save(CaptureProductionOutputPreferences());
        }
        catch (Exception)
        {
            // Saving preferences must never interrupt a live production operation.
        }
    }

    private ProductionOutputPreferences CaptureProductionOutputPreferences() =>
        new()
        {
            Version = 2,
            FfmpegBinDirectory = FfmpegBinDirectory,
            StreamRtmpEnabled = StreamRtmpEnabled,
            StreamNdiEnabled = StreamNdiEnabled,
            StreamSrtEnabled = StreamSrtEnabled,
            StreamRtmpProtocol = StreamRtmpProtocol,
            StreamRtmpServerUrl = StreamRtmpServerUrl,
            StreamRtmpStreamKey = StreamRtmpStreamKey,
            StreamNdiProgramName = StreamNdiProgramName,
            StreamNdiGroupName = StreamNdiGroupName,
            StreamSrtMode = StreamSrtMode,
            StreamSrtHost = StreamSrtHost,
            StreamSrtPort = StreamSrtPort,
            StreamSrtLatencyMs = StreamSrtLatencyMs,
            StreamSrtStreamId = StreamSrtStreamId,
            StreamSrtKeyLength = StreamSrtKeyLength,
            StreamSrtPassphrase = StreamSrtPassphrase,
            CanvasResolution = CanvasResolution,
            CanvasFps = CanvasFps,
            LocalAudioSourceEnabled = LocalAudioSourceEnabled,
            SelectedLocalAudioCaptureDeviceId = SelectedLocalAudioCaptureDeviceId,
            AudioMonitoringEnabled = AudioMonitoringEnabled,
            SelectedAudioMonitorDeviceId = SelectedAudioMonitorDeviceId,
            AudioMonitorVolume = NormalizeAudioMonitorVolume(AudioMonitorVolume),
            StreamRenderResolution = StreamRenderResolution,
            StreamRenderFps = StreamRenderFps,
            StreamVideoCodec = StreamVideoCodec,
            StreamTargetBitrateMbps = NormalizeStreamTargetBitrateMbps(StreamTargetBitrateMbps),
            StreamAudioBitrateKbps = NormalizeAudioBitrateKbps(StreamAudioBitrateKbps),
            StreamEncoderMode = StreamEncoderMode,
            StreamPlatformProfileId = StreamPlatformProfileId,
            StreamKeyframeIntervalSeconds = Math.Clamp(StreamKeyframeIntervalSeconds, 0.5, 10),
            StreamRateControl = NormalizeStreamRateControl(StreamRateControl),
            StreamH264Profile = NormalizeStreamH264Profile(StreamH264Profile),
            StreamBFrames = (int)Math.Clamp(Math.Round(StreamBFrames), 0, 4),
            StreamAllowEnhancedRtmp = StreamAllowEnhancedRtmp,
            RecordingRenderResolution = RecordingRenderResolution,
            RecordingRenderFps = RecordingRenderFps,
            RecordingVideoCodec = RecordingVideoCodec,
            RecordingTargetBitrateMbps = NormalizeOutputTargetBitrateMbps(RecordingTargetBitrateMbps),
            RecordingAudioBitrateKbps = NormalizeAudioBitrateKbps(RecordingAudioBitrateKbps),
            RecordingTargetFolder = RecordingTargetFolder,
            RecordingFilenamePrefix = RecordingFilenamePrefix,
            RecordingFormat = RecordingFormat,
            RecordingQuality = RecordingQuality,
            LowerThirdPosition = Overlays.LowerThirdPosition,
            LowerThirdBuildInMs = NormalizeLowerThirdTimingMs(LowerThirdBuildInMs),
            LowerThirdBuildOutMs = NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs),
            BrandLowerThirdStyle = BrandKit.LowerThirdStyle,
            BrandDefaultOverlayBehavior = BrandKit.DefaultOverlayBehavior,
            MultiviewLayoutMode = NormalizeMultiviewLayoutMode(MultiviewLayoutMode),
            MultiviewTileCount = ClampMultiviewTileCount(MultiviewTileCount),
            MultiviewShowLabels = MultiviewShowLabels,
            MultiviewShowTally = MultiviewShowTally,
            MultiviewShowMeters = MultiviewShowMeters,
            MultiviewShowClock = MultiviewShowClock,
            SceneBackgroundAssetIds = _sceneBackgroundAssetIds.ToDictionary(
                pair => pair.Key,
                pair => pair.Value,
                StringComparer.Ordinal),
            SourceDisplayNames = _sourceDisplayNames.ToDictionary(
                pair => pair.Key,
                pair => pair.Value,
                StringComparer.Ordinal),
            CustomScenes = _scenes
                .Where(scene => scene.Id.StartsWith("custom-", StringComparison.Ordinal))
                .Select(scene => ScenePersistenceService.ToPersisted(
                    scene,
                    _sceneRoutes.TryGetValue(scene.Id, out var routes) ? routes : []))
                .ToList()
        };

    private void ApplyProductionOutputPreferences(ProductionOutputPreferences preferences)
    {
        FfmpegBinDirectory = preferences.FfmpegBinDirectory ?? FfmpegBinDirectory;
        StreamRtmpEnabled = preferences.StreamRtmpEnabled;
        StreamNdiEnabled = preferences.StreamNdiEnabled;
        StreamSrtEnabled = preferences.StreamSrtEnabled;
        StreamRtmpProtocol = preferences.StreamRtmpProtocol ?? StreamRtmpProtocol;
        StreamRtmpServerUrl = preferences.StreamRtmpServerUrl ?? StreamRtmpServerUrl;
        StreamRtmpStreamKey = preferences.StreamRtmpStreamKey ?? StreamRtmpStreamKey;
        StreamNdiProgramName = preferences.StreamNdiProgramName ?? StreamNdiProgramName;
        StreamNdiGroupName = preferences.StreamNdiGroupName ?? StreamNdiGroupName;
        StreamSrtMode = preferences.StreamSrtMode ?? StreamSrtMode;
        StreamSrtHost = preferences.StreamSrtHost ?? StreamSrtHost;
        StreamSrtPort = preferences.StreamSrtPort ?? StreamSrtPort;
        StreamSrtLatencyMs = preferences.StreamSrtLatencyMs ?? StreamSrtLatencyMs;
        StreamSrtStreamId = preferences.StreamSrtStreamId ?? StreamSrtStreamId;
        StreamSrtKeyLength = preferences.StreamSrtKeyLength ?? StreamSrtKeyLength;
        StreamSrtPassphrase = preferences.StreamSrtPassphrase ?? StreamSrtPassphrase;
        CanvasResolution = preferences.CanvasResolution ?? CanvasResolution;
        CanvasFps = preferences.CanvasFps ?? CanvasFps;
        _localAudioSourceEnabled = preferences.LocalAudioSourceEnabled;
        _selectedLocalAudioCaptureDeviceId = preferences.SelectedLocalAudioCaptureDeviceId ?? SelectedLocalAudioCaptureDeviceId;
        _audioMonitoringEnabled = preferences.AudioMonitoringEnabled;
        _selectedAudioMonitorDeviceId = preferences.SelectedAudioMonitorDeviceId ?? SelectedAudioMonitorDeviceId;
        _audioMonitorVolume = NormalizeAudioMonitorVolume(preferences.AudioMonitorVolume);
        StreamRenderResolution = preferences.StreamRenderResolution ?? StreamRenderResolution;
        StreamRenderFps = preferences.StreamRenderFps ?? StreamRenderFps;
        StreamVideoCodec = preferences.StreamVideoCodec ?? StreamVideoCodec;
        StreamTargetBitrateMbps = preferences.StreamTargetBitrateMbps > 0
            ? NormalizeStreamTargetBitrateMbps(preferences.StreamTargetBitrateMbps)
            : StreamTargetBitrateMbps;
        StreamAudioBitrateKbps = preferences.StreamAudioBitrateKbps > 0
            ? NormalizeAudioBitrateKbps(preferences.StreamAudioBitrateKbps)
            : StreamAudioBitrateKbps;
        StreamEncoderMode = preferences.StreamEncoderMode ?? StreamEncoderMode;
        StreamKeyframeIntervalSeconds = preferences.StreamKeyframeIntervalSeconds > 0
            ? Math.Clamp(preferences.StreamKeyframeIntervalSeconds, 0.5, 10)
            : StreamKeyframeIntervalSeconds;
        StreamRateControl = NormalizeStreamRateControl(preferences.StreamRateControl);
        StreamH264Profile = NormalizeStreamH264Profile(preferences.StreamH264Profile);
        StreamBFrames = Math.Clamp(preferences.StreamBFrames, 0, 4);
        StreamAllowEnhancedRtmp = preferences.StreamAllowEnhancedRtmp;
        StreamPlatformProfileId = StreamingProfileCatalog.Find(preferences.StreamPlatformProfileId).Id;
        RecordingRenderResolution = preferences.RecordingRenderResolution ?? RecordingRenderResolution;
        RecordingRenderFps = preferences.RecordingRenderFps ?? RecordingRenderFps;
        RecordingVideoCodec = preferences.RecordingVideoCodec ?? RecordingVideoCodec;
        RecordingTargetBitrateMbps = preferences.RecordingTargetBitrateMbps > 0
            ? NormalizeOutputTargetBitrateMbps(preferences.RecordingTargetBitrateMbps)
            : RecordingTargetBitrateMbps;
        RecordingAudioBitrateKbps = preferences.RecordingAudioBitrateKbps > 0
            ? NormalizeAudioBitrateKbps(preferences.RecordingAudioBitrateKbps)
            : RecordingAudioBitrateKbps;
        RecordingTargetFolder = preferences.RecordingTargetFolder ?? RecordingTargetFolder;
        RecordingFilenamePrefix = preferences.RecordingFilenamePrefix ?? RecordingFilenamePrefix;
        RecordingFormat = preferences.RecordingFormat ?? RecordingFormat;
        RecordingQuality = preferences.RecordingQuality ?? RecordingQuality;
        LowerThirdBuildInMs = preferences.LowerThirdBuildInMs > 0
            ? NormalizeLowerThirdTimingMs(preferences.LowerThirdBuildInMs)
            : LowerThirdBuildInMs;
        LowerThirdBuildOutMs = preferences.LowerThirdBuildOutMs > 0
            ? NormalizeLowerThirdTimingMs(preferences.LowerThirdBuildOutMs)
            : LowerThirdBuildOutMs;

        if (!string.IsNullOrWhiteSpace(preferences.LowerThirdPosition))
        {
            Overlays.LowerThirdPosition = preferences.LowerThirdPosition;
        }

        if (!string.IsNullOrWhiteSpace(preferences.BrandLowerThirdStyle) ||
            !string.IsNullOrWhiteSpace(preferences.BrandDefaultOverlayBehavior))
        {
            BrandKit = new BrandKit
            {
                Name = BrandKit.Name,
                LogoText = BrandKit.LogoText,
                LogoAssetId = BrandKit.LogoAssetId,
                LogoAssetName = BrandKit.LogoAssetName,
                LogoAssetPath = BrandKit.LogoAssetPath,
                BrandColor = BrandKit.BrandColor,
                AccentColor = BrandKit.AccentColor,
                BackgroundColor = BrandKit.BackgroundColor,
                FontFamily = BrandKit.FontFamily,
                LowerThirdStyle = string.IsNullOrWhiteSpace(preferences.BrandLowerThirdStyle)
                    ? BrandKit.LowerThirdStyle
                    : preferences.BrandLowerThirdStyle,
                CaptionStyle = BrandKit.CaptionStyle,
                DefaultOverlayBehavior = string.IsNullOrWhiteSpace(preferences.BrandDefaultOverlayBehavior)
                    ? BrandKit.DefaultOverlayBehavior
                    : preferences.BrandDefaultOverlayBehavior
            };
            Overlays.NotifyBrandKitChanged();
        }

        MultiviewLayoutMode = NormalizeMultiviewLayoutMode(preferences.MultiviewLayoutMode);
        MultiviewTileCount = ClampMultiviewTileCount(preferences.MultiviewTileCount);
        MultiviewShowLabels = preferences.MultiviewShowLabels;
        MultiviewShowTally = preferences.MultiviewShowTally;
        MultiviewShowMeters = preferences.MultiviewShowMeters;
        MultiviewShowClock = preferences.MultiviewShowClock;

        _sceneBackgroundAssetIds.Clear();
        foreach (var pair in preferences.SceneBackgroundAssetIds)
        {
            if (!string.IsNullOrWhiteSpace(pair.Key) && !string.IsNullOrWhiteSpace(pair.Value))
            {
                _sceneBackgroundAssetIds[pair.Key] = pair.Value;
            }
        }

        _sourceDisplayNames.Clear();
        foreach (var pair in preferences.SourceDisplayNames)
        {
            if (!string.IsNullOrWhiteSpace(pair.Key) && !string.IsNullOrWhiteSpace(pair.Value))
            {
                _sourceDisplayNames[pair.Key] = pair.Value.Trim();
            }
        }

        // Custom scenes survive restarts (scenes redesign S2). Restore only
        // custom-* ids that aren't already present, so built-ins and any scene
        // created earlier in this session win over the persisted copy.
        var restoredScenes = 0;
        foreach (var persisted in preferences.CustomScenes)
        {
            if (string.IsNullOrWhiteSpace(persisted.Id) ||
                !persisted.Id.StartsWith("custom-", StringComparison.Ordinal) ||
                _scenes.Any(scene => string.Equals(scene.Id, persisted.Id, StringComparison.Ordinal)))
            {
                continue;
            }

            _scenes.Add(ScenePersistenceService.SceneFromPersisted(persisted));
            _sceneRoutes[persisted.Id] = persisted.Routes
                .Select(ScenePersistenceService.FromPersisted)
                .ToList();
            restoredScenes++;
        }

        if (restoredScenes > 0)
        {
            RefreshSceneItems();
        }

        RefreshAudioMonitorBindings();
        RefreshSceneBackgroundSelection();
    }

    // Persistence for Input 1-10 slot assignments (alpha #2). The store is abstracted so the
    // round-trip is unit-testable without a running WinUI/ApplicationData host.
    private static IShowInputRosterStore CreateShowInputRosterStore()
    {
        try
        {
            var folder = Windows.Storage.ApplicationData.Current.LocalFolder.Path;
            return new FileShowInputRosterStore(folder);
        }
        catch (Exception)
        {
            // No packaged ApplicationData (e.g. unpackaged/test host) — fall back to a
            // local-folder JSON file when possible, otherwise in-memory.
            try
            {
                var folder = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "CoreVideoPro");
                return new FileShowInputRosterStore(folder);
            }
            catch (Exception)
            {
                return new InMemoryShowInputRosterStore();
            }
        }
    }

    private void LoadShowInputRoster()
    {
        try
        {
            var snapshot = _showInputRosterStore.Load();
            // Intent is restored even if the referenced participant/device is no longer present;
            // RefreshShowInputEditors / BuildMultiviewTiles surface unresolved sources as
            // unavailable rather than dropping the assignment.
            ShowInputRosterSerializer.ApplyTo(ShowInputs, snapshot);
        }
        catch (Exception)
        {
            // Persistence is best-effort; never block startup on a bad/locked store.
        }
        finally
        {
            _showInputRosterLoaded = true;
        }
    }

    private void SaveShowInputRoster()
    {
        // Guard against persisting before the saved roster has been loaded/applied.
        if (!_showInputRosterLoaded)
        {
            return;
        }

        try
        {
            _showInputRosterStore.Save(ShowInputRosterSerializer.CaptureFrom(ShowInputs));
        }
        catch (Exception)
        {
            // Best-effort; a failed save must not surface as a user-facing error.
        }
    }

    private void InitializeShowInputEditors()
    {
        ShowInputEditors.Clear();
        foreach (var slot in ShowInputs)
        {
            ShowInputEditors.Add(new ShowInputSlotViewModel(
                slot,
                OnShowInputChanged,
                SetCaptureDeviceAudioSource,
                ResolveSourceDisplayName,
                SetSourceDisplayName));
        }

        RefreshShowInputEditors(force: true);
    }

    private string _showInputEditorsSignature = "";

    private void RefreshShowInputEditors(bool force = false)
    {
        EnsureAssignedScreensConnected();
        var mediaAssets = MediaBinGroups.SelectMany(group => group.Assets).ToList();

        // Rebuild each slot's Source dropdown options ONLY when the set the picker
        // depends on actually changes. This method is called from many per-snapshot
        // paths (~10/s); rebuilding the ComboBox ItemsSource every tick resets the
        // operator's in-progress selection (can't pick a participant) and fires a
        // SelectionChanged storm. Skip when nothing changed.
        //
        // The picker membership depends only on WHO is in the room (participant ids) +
        // devices + assets — NOT on participant Health. Health (talking/active-speaker/
        // camera on-off) churns constantly during a live meeting; including it here
        // rebuilt every slot's ItemsSource on every health tick, which blanked the
        // selected Source/name in the picker and flooded the dispatcher (a churn/crash
        // vector). Key the signature on id-set only so options rebuild on join/leave.
        var signature =
            string.Join("|", RoomParticipantsForInputs.Select(p => p.Id)) + "#" +
            string.Join(",", CaptureDevices.Select(d => d.Id)) + "#" +
            AudioCaptureDevices.Count + "#" + mediaAssets.Count;
        if (!force && signature == _showInputEditorsSignature)
        {
            return;
        }
        _showInputEditorsSignature = signature;

        foreach (var editor in ShowInputEditors)
        {
            editor.RefreshSourceOptions(RoomParticipantsForInputs, CaptureDevices, AudioCaptureDevices, mediaAssets);
        }

        OnPropertyChanged(nameof(ShowInputSummary));
        OnPropertyChanged(nameof(MultiviewHeader));
        NotifyShowReadinessChanged();
    }

    private void OnShowInputChanged() => ScheduleShowInputRefresh();

    private void ScheduleShowInputRefresh()
    {
        if (_showInputRefreshScheduled)
        {
            return;
        }

        _showInputRefreshScheduled = true;
        _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _showInputRefreshScheduled = false;
            ApplyShowInputRefresh();
        });
    }

    private void ApplyShowInputRefresh()
    {
        EnsureAssignedSlotsForInShow();

        var active = ShowInputs.Where(slot => slot.InShow).ToList();
        if (active.Count > ShowInputRosterService.MaxMultiviewBoxes)
        {
            foreach (var slot in active.Skip(ShowInputRosterService.MaxMultiviewBoxes))
            {
                slot.InShow = false;
            }
        }

        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
        RefreshRoutingMatricesIfVisible();
        SchedulePreviewRoutingRefresh();
        QueueSelectedCaptureDevicesOnline();
        SaveShowInputRoster();
        CommandStatus = "Show input roster updated";
    }

    private void QueueSelectedCaptureDevicesOnline()
    {
        foreach (var deviceId in ShowInputs
                     .Where(slot => slot.InShow &&
                         (slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.Screen or ShowInputKind.SrtIngest) &&
                         !string.IsNullOrWhiteSpace(slot.CaptureDeviceId))
                     .Select(slot => slot.CaptureDeviceId!)
                     .Distinct(StringComparer.Ordinal))
        {
            var device = ResolveCaptureDevice(deviceId);
            if (device is null ||
                device.ConnectionState is CaptureConnectionState.Connected or CaptureConnectionState.Error ||
                !_captureAutoConnectInFlight.Add(deviceId))
            {
                continue;
            }

            _ = ConnectSelectedCaptureDeviceAsync(deviceId);
        }
    }

    private async Task ConnectSelectedCaptureDeviceAsync(string deviceId)
    {
        var enteredGate = false;
        try
        {
            await _captureConnectGate.WaitAsync().ConfigureAwait(false);
            enteredGate = true;
            await ConnectCaptureDeviceAsync(deviceId).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"capture: auto-connect failed {deviceId}: {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            if (enteredGate)
            {
                _captureConnectGate.Release();
            }

            RunOnUiThread(() => _captureAutoConnectInFlight.Remove(deviceId));
        }
    }

    private MediaAsset? FindMediaAsset(string assetId) =>
        MediaBinGroups
            .SelectMany(group => group.Assets)
            .FirstOrDefault(candidate => string.Equals(candidate.Id, assetId, StringComparison.Ordinal));

    private MediaAsset? TryResolveRouteMediaAsset(SourceRoute route) =>
        route.Mode == SourceRouteMode.Fixed &&
        ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var mediaAssetId)
            ? FindMediaAsset(mediaAssetId)
            : null;

    private bool ShouldAutoPlayMediaRoute(string mediaAssetId, bool isProgramScene) =>
        ShouldAutoPlayMediaRoute(mediaAssetId, isProgramScene, GetResolvedProgramRoutes());

    private bool ShouldAutoPlayMediaRoute(
        string mediaAssetId,
        bool isProgramScene,
        IReadOnlyList<SourceRoute> programRoutes) =>
        MediaRoutePlaybackService.ShouldPlaySceneMediaRoute(
            mediaAssetId,
            isProgramScene,
            SelectedMediaAssetId,
            SelectedMediaAssetPlaying,
            programRoutes);

    private IReadOnlyList<SourceRoute> GetResolvedProgramRoutes() =>
        GetMutableRoutes(ActiveSceneId)
            .Select(ResolveRouteFromShowInput)
            .ToList();

    public static string BuildProgramMediaRouteSignature(IReadOnlyList<SourceRoute> routes)
    {
        var mediaAssetIds = routes
            .Select(route =>
            {
                var resolved = route.Mode == SourceRouteMode.Fixed &&
                    ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var mediaAssetId)
                        ? mediaAssetId
                        : null;
                return string.IsNullOrWhiteSpace(resolved) ? null : resolved;
            })
            .OfType<string>()
            .GroupBy(value => value, StringComparer.Ordinal)
            .Select(group => $"{group.Key}:{group.Count()}")
            .OrderBy(value => value, StringComparer.Ordinal)
            .ToList();

        return string.Join("|", mediaAssetIds);
    }

    private static bool IsVisualMediaAsset(MediaAsset asset)
    {
        var kind = asset.Kind.ToLowerInvariant();
        if (kind.Contains("audio", StringComparison.Ordinal))
        {
            return false;
        }

        var extension = Path.GetExtension(asset.FilePath);
        return extension.Equals(".mp4", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".mov", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".webm", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".png", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpg", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".jpeg", StringComparison.OrdinalIgnoreCase) ||
            extension.Equals(".gif", StringComparison.OrdinalIgnoreCase);
    }

    private MediaAsset? ResolveSceneBackgroundAsset(string sceneId)
    {
        if (!_sceneBackgroundAssetIds.TryGetValue(sceneId, out var assetId))
        {
            return null;
        }

        var asset = FindMediaAsset(assetId);
        return asset is not null && IsVisualMediaAsset(asset) ? asset : null;
    }

    private void PruneMissingSceneBackgrounds()
    {
        SceneBackgroundSelectionService.PruneMissingBackgrounds(
            _sceneBackgroundAssetIds,
            FindMediaAsset,
            IsVisualMediaAsset);
    }

    private void RefreshSceneBackgroundSelection()
    {
        _refreshingSceneBackgroundSelection = true;
        try
        {
            PreviewSceneBackgroundAssetId = SceneBackgroundSelectionService.ResolveSelectedAssetId(
                PreviewSceneId,
                _sceneBackgroundAssetIds,
                FindMediaAsset,
                IsVisualMediaAsset);
        }
        finally
        {
            _refreshingSceneBackgroundSelection = false;
        }

        NotifySceneBackgroundChanged();
    }

    private void NotifySceneBackgroundChanged()
    {
        OnPropertyChanged(nameof(PreviewSceneBackgroundAssetId));
        OnPropertyChanged(nameof(PreviewSceneBackgroundAsset));
        OnPropertyChanged(nameof(ProgramSceneBackgroundAsset));
        OnPropertyChanged(nameof(SuperSourceBackgroundSummary));
    }

    private MediaCoreSceneBackgroundWire? BuildSceneBackgroundWire(string sceneId)
    {
        var asset = ResolveSceneBackgroundAsset(sceneId);
        return asset is null
            ? null
            : new MediaCoreSceneBackgroundWire(
                asset.Id,
                asset.Name,
                asset.Kind,
                asset.FilePath,
                Playing: true);
    }

    private IReadOnlyList<MediaBinGroup> ApplyMediaSelection(IReadOnlyList<MediaBinGroup> groups) =>
        groups.Select(group => new MediaBinGroup
        {
            Kind = group.Kind,
            Label = group.Label,
            Assets = group.Assets.Select(asset => new MediaAsset
            {
                Id = asset.Id,
                Name = asset.Name,
                Kind = asset.Kind,
                DurationMs = asset.DurationMs,
                NaturalWidth = asset.NaturalWidth,
                NaturalHeight = asset.NaturalHeight,
                RelativePath = asset.RelativePath,
                FilePath = asset.FilePath,
                FileType = asset.FileType,
                IsSelected = string.Equals(asset.Id, SelectedMediaAssetId, StringComparison.Ordinal),
                IsPlaying = SelectedMediaAssetPlaying &&
                    string.Equals(asset.Id, SelectedMediaAssetId, StringComparison.Ordinal)
            }).ToList()
        }).ToList();

    private void RefreshRoutingMatricesIfVisible()
    {
        if (ActiveTab != StudioTab.Routing)
        {
            return;
        }

        BuildAudioRoutingMatrix();
        BuildVideoRoutingMatrix();
    }

    private void EnsureAssignedSlotsForInShow()
    {
        var defaultParticipant = RoomVideoParticipants.FirstOrDefault()?.Id;
        var defaultCapture = CaptureDevices.FirstOrDefault(device => device.IsConnected) ??
            CaptureDevices.FirstOrDefault();

        foreach (var slot in ShowInputs.Where(slot => slot.InShow && !slot.IsAssigned))
        {
            if (!string.IsNullOrWhiteSpace(defaultParticipant))
            {
                slot.Kind = ShowInputKind.ZoomParticipant;
                slot.ParticipantId = defaultParticipant;
            }
            else if (defaultCapture is not null)
            {
                slot.Kind = ResolveShowInputKind(defaultCapture);
                slot.CaptureDeviceId = defaultCapture.Id;
            }
        }
    }

    // Screens connect CORE-side and ONLY on operator intent; but intent is
    // expressed by ASSIGNMENT (any picker path), not by a connect button - an
    // assigned screen that is not streaming is always wrong (owner-reported:
    // screen in the multiviewer, no frames). Idempotent; in-flight guarded.
    private readonly HashSet<string> _screenConnectsInFlight = [];

    private void EnsureAssignedScreensConnected()
    {
        foreach (var device in CaptureDevices)
        {
            if ((!device.Id.StartsWith("screen:", StringComparison.Ordinal) && !device.Id.StartsWith("window:", StringComparison.Ordinal)) ||
                device.ConnectionState == CaptureConnectionState.Connected ||
                _screenConnectsInFlight.Contains(device.Id))
            {
                continue;
            }
            var assigned = ShowInputs.Any(slot =>
                string.Equals(slot.CaptureDeviceId, device.Id, StringComparison.Ordinal));
            if (!assigned)
            {
                continue;
            }
            _screenConnectsInFlight.Add(device.Id);
            LaunchLog.Write($"capture: auto-connecting assigned screen {device.Id}");
            _ = ConnectAssignedScreenAsync(device.Id);
        }
    }

    private async Task ConnectAssignedScreenAsync(string deviceId)
    {
        try
        {
            await ConnectCaptureDeviceAsync(deviceId).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"capture: screen auto-connect failed {deviceId}: {ex.Message}");
        }
        finally
        {
            RunOnUiThread(() => _screenConnectsInFlight.Remove(deviceId));
        }
    }

    // Lifecycle L2: take a device offline - stop its core session (WGC/SRT),
    // unassign every slot that references it, refresh. Webcams using the WinUI
    // MediaCapture bridge keep their existing capture-toggle path; this covers
    // the core-session kinds (screens today).
    [RelayCommand]
    private async Task TakeCaptureDeviceOfflineAsync(string deviceId)
    {
        if (string.IsNullOrWhiteSpace(deviceId))
        {
            return;
        }
        LaunchLog.Write(string.Format("lifecycle: take offline {0}", deviceId));
        try
        {
            var statuses = await _bridge.DisconnectNativeCaptureDeviceAsync(deviceId).ConfigureAwait(false);
            var match = statuses.FirstOrDefault(s => s.Id == deviceId);
            RunOnUiThread(() =>
            {
                var device = CaptureDevices.FirstOrDefault(d => d.Id == deviceId);
                if (device is not null)
                {
                    device.ConnectionState = CaptureConnectionState.Detected;
                    device.SignalPresent = match?.SignalPresent ?? false;
                }
                foreach (var editor in ShowInputEditors.Where(e => string.Equals(e.CaptureDeviceId, deviceId, StringComparison.Ordinal)).ToList())
                {
                    LaunchLog.Write(string.Format("lifecycle: offline {0} unassigns slot {1}", deviceId, editor.SlotNumber));
                    editor.Unassign();
                }
                RefreshShowInputEditors(force: true);
                RefreshDualCaptureSourceOptions();
                RefreshCaptureFleetSummary();
                RefreshPreviewRoutingState();
                RefreshMultiviewGridTiles();
                CommandStatus = string.Format("{0} is offline.", device?.Name ?? deviceId);
                _ = TrySyncMediaCoreAsync();
            });
        }
        catch (Exception ex)
        {
            LaunchLog.Write(string.Format("lifecycle: take offline failed {0}: {1}", deviceId, ex.Message));
        }
    }

    // Lifecycle L1: operator-facing unassign (button on each Show Input row).
    [RelayCommand]
    private void UnassignShowInput(ShowInputSlotViewModel editor)
    {
        if (editor is null)
        {
            return;
        }
        LaunchLog.Write(string.Format("lifecycle: unassign slot {0} (was {1} cap={2} pid={3})",
            editor.SlotNumber, editor.Kind, editor.CaptureDeviceId ?? "-", editor.ParticipantId ?? "-"));
        editor.Unassign();
        RefreshShowInputEditors(force: true);
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
        _ = TrySyncMediaCoreAsync();
    }

    // Lifecycle L1: startup/refresh ghost sweep - parked slots whose source is
    // gone or which duplicate a lower slot get cleared automatically, LOGGED.
    private void SweepGhostShowInputAssignments()
    {
        var swept = 0;
        foreach (var slot in ShowInputs)
        {
            if (slot.InShow || !slot.IsAssigned)
            {
                continue;
            }
            if (IsShowInputSourceStale(slot) || IsDuplicateShowInputAssignment(slot))
            {
                LaunchLog.Write(string.Format("lifecycle: sweep ghost slot {0} (kind={1} cap={2} pid={3})",
                    slot.SlotNumber, slot.Kind, slot.CaptureDeviceId ?? "-", slot.ParticipantId ?? "-"));
                slot.Kind = ShowInputKind.Unassigned;
                swept++;
            }
        }

        // Lifecycle L7 (source-lifecycle-spec): persist the cleaned roster so
        // ghosts never survive two sessions. Without this, the sweep cleared
        // slots at runtime but the stale records stayed in the JSON file and
        // came back on every launch (the exact accumulation that produced the
        // 8-ghost roster the owner hit).
        if (swept > 0)
        {
            LaunchLog.Write(string.Format("lifecycle: persistence GC rewrote roster after {0} ghost(s)", swept));
            SaveShowInputRoster();
        }
    }

    // A slot is STALE when its assigned source no longer exists anywhere we
    // can see - the device left the roster or the participant left the call.
    private bool IsShowInputSourceStale(ShowInputSlot slot)
    {
        if (!slot.IsAssigned)
        {
            return false;  // empty, not stale (handled by the free-slot search)
        }
        if (!string.IsNullOrWhiteSpace(slot.CaptureDeviceId))
        {
            return CaptureDevices.All(d => !string.Equals(d.Id, slot.CaptureDeviceId, StringComparison.Ordinal));
        }
        if (!string.IsNullOrWhiteSpace(slot.ParticipantId))
        {
            return RoomVideoParticipants.All(p => !string.Equals(p.Id, slot.ParticipantId, StringComparison.Ordinal));
        }
        return false;
    }

    private bool IsDuplicateShowInputAssignment(ShowInputSlot slot)
    {
        if (!slot.IsAssigned)
        {
            return false;
        }
        return ShowInputs.Any(other =>
            other.SlotNumber < slot.SlotNumber &&
            other.IsAssigned &&
            string.Equals(other.CaptureDeviceId, slot.CaptureDeviceId, StringComparison.Ordinal) &&
            string.Equals(other.ParticipantId, slot.ParticipantId, StringComparison.Ordinal));
    }

    private void AssignConnectedCaptureDeviceToShowInput(CaptureDevice device)
    {
        var targetKind = ResolveShowInputKind(device);
        LaunchLog.Write(string.Format("assign: {0} kind={1} slots={2} freeInShow={3} freeParked={4}", device.Id, targetKind,
            ShowInputs.Count,
            ShowInputs.Count(s => s.InShow && !s.IsAssigned),
            ShowInputs.Count(s => !s.InShow && !s.IsAssigned)));
        var existing = ShowInputs.FirstOrDefault(slot =>
            slot.Kind == targetKind &&
            string.Equals(slot.CaptureDeviceId, device.Id, StringComparison.Ordinal));
        if (existing is not null)
        {
            existing.InShow = true;
            return;
        }

        var slot = ShowInputs.FirstOrDefault(slot => slot.InShow && !slot.IsAssigned) ??
            ShowInputs.FirstOrDefault(slot => !slot.InShow && !slot.IsAssigned) ??
            // Lifecycle reclaim: slots hold assignments forever (nothing cleans
            // up when a device/participant disappears), so a full roster of
            // GHOSTS silently blocked every new source (owner-reported: ten
            // stale slots, freeParked=0). Reuse a slot whose source no longer
            // exists - parked ones first, then in-show ghosts.
            ShowInputs.FirstOrDefault(slot => !slot.InShow && IsShowInputSourceStale(slot)) ??
            // Duplicate ghosts: persistence-era damage left MANY parked slots
            // pointing at one device (rig-measured: 8 of 10 on the same cam).
            // A parked duplicate of a lower slot is always reclaimable.
            ShowInputs.FirstOrDefault(slot => !slot.InShow && IsDuplicateShowInputAssignment(slot)) ??
            ShowInputs.FirstOrDefault(slot => IsShowInputSourceStale(slot));
        if (slot is null)
        {
            LaunchLog.Write(string.Format("assign: {0} REJECTED - all {1} slots hold live sources", device.Id, ShowInputs.Count));
            foreach (var s in ShowInputs)
            {
                LaunchLog.Write(string.Format("assign: slot{0} kind={1} cap={2} pid={3} inShow={4}", s.SlotNumber, s.Kind, s.CaptureDeviceId ?? "-", s.ParticipantId ?? "-", s.InShow));
            }
            CommandStatus = "All Show Inputs are assigned to live sources. Unassign one on the Inputs tab first.";
            return;
        }
        LaunchLog.Write(string.Format("assign: {0} -> slot {1}", device.Id, slot.SlotNumber));

        slot.Kind = targetKind;
        slot.CaptureDeviceId = device.Id;
        slot.InShow = true;
    }

    private static ShowInputKind ResolveShowInputKind(CaptureDevice device) =>
        device.Vendor.ToLowerInvariant() switch
        {
            "blackmagic" => ShowInputKind.Blackmagic,
            "aja" => ShowInputKind.Aja,
            "srt" => ShowInputKind.SrtIngest,
            "uvc" or "windows" => ShowInputKind.UvcWebcam,
            "screen capture" => ShowInputKind.Screen,
            _ => ShowInputKind.UvcWebcam
        };

    private static bool IsVirtualSrtIngestDevice(CaptureDevice device) =>
        string.Equals(device.Vendor, "srt", StringComparison.OrdinalIgnoreCase);

    private void RefreshMultiviewGridTiles()
    {
        // MUST run on the UI thread: it assigns MultiviewGridTiles (x:Bound to the grid's
        // ItemsRepeater) + OnPropertyChanged, and reads UI-owned collections (ShowInputs,
        // RoomParticipantsForInputs, MultiviewTiles). It is called from ~20 sites including the
        // roster/active-speaker churn path (~1.3x/s under a live meeting); if even one
        // fires off the UI thread the bound-prop set is a native fail-fast
        // (CoreMessagingXP +0x93b66 / 0xc000027b, no managed stack — reproduced under
        // sustained multi-participant churn). Marshal here so every caller is safe; runs
        // inline when already on the UI thread.
        if (!_dispatcher.HasThreadAccess)
        {
            _dispatcher.TryEnqueue(RefreshMultiviewGridTiles);
            return;
        }

        // Resolve Zoom slots against the FULL in-room roster (RoomParticipantsForInputs), not
        // the video-only subset (RoomVideoParticipants). The Sources picker assigns from the
        // full roster, so a slot can be bound to a participant whose camera is momentarily off.
        // Resolving here against the video-only list silently dropped that slot from the
        // multiview, and because the grid packs sequentially every later slot then shifted -
        // surfacing as the reported "wrong / stale / missing source". Passing the full roster
        // lets ToSurfaceTile emit a waiting/video-off placeholder in the slot's own position,
        // so the operator's routing is honored deterministically and matches the Sources screen.
        // Live video frames still flow from MultiviewTiles, which is built from the video-only
        // participants, so a camera-on participant still gets their live surface.
        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            ShowInputs,
            RoomParticipantsForInputs,
            CaptureDevices,
            MultiviewTiles,
            _surfaces.CaptureDeviceSurfaces,
            VisualMediaAssets).ToList();

        if (SelectedMediaAssetId is not null && FindMediaAsset(SelectedMediaAssetId) is { } asset)
        {
            tiles.Add(new ParticipantSurfaceTile
            {
                Participant = new Participant
                {
                    Id = $"media:{asset.Id}",
                    Name = asset.Name,
                    Title = asset.Kind,
                    Role = ParticipantRole.Guest,
                    Health = SelectedMediaAssetPlaying ? FeedHealth.Live : FeedHealth.VideoOff
                },
                Surface = VideoSurfaceState.MediaAssetPreview(
                    $"media:{asset.Id}",
                    asset.Name,
                    asset.FilePath,
                    asset.Kind,
                    SelectedMediaAssetPlaying,
                    naturalSourceWidth: asset.NaturalWidth,
                    naturalSourceHeight: asset.NaturalHeight),
                SourceIndex = tiles.Count + 1
            });
        }

        MultiviewGridTiles = tiles;
        OnPropertyChanged(nameof(MultiviewHeader));

        // The Show Input roster also drives the core-composited GPU multiview. These are the
        // structural-change points, so (re)send set-multiview-layout here (deduped). The old
        // per-tile CPU multiview is retired — MultiviewGridTiles now only feeds the header count.
        SyncMultiviewLayoutIfChanged();
    }

    private void RefreshOutputStatus()
    {
        // Recording/streaming run against the always-on core, so reflect the live snapshot
        // whenever the core is running — capture does not have to be subscribed.
        if (_bridge.Running && _bridge.LastSnapshot is { } snapshot)
        {
            OutputStatus = MediaCoreBridgeService.SummarizeOutputs(snapshot);
            OutputSessionStatus = LiveProductionSync.SummarizeOutputSession(snapshot);
            NotifyShowReadinessChanged();
            return;
        }

        if (!Recording && !Streaming)
        {
            OutputStatus = "Outputs idle";
            OutputSessionStatus = "Outputs idle";
            NotifyShowReadinessChanged();
            return;
        }

        var localStatus = LiveProductionSync.SummarizeLocalOutputs(Recording, Streaming);
        OutputStatus = localStatus;
        OutputSessionStatus = localStatus;
        NotifyShowReadinessChanged();
    }

    private void RefreshTransportState()
    {
        RefreshTransportAutomationState();
        Transport.ApplyMeetingState(Settings.IsInMeeting);

        if (_bridge.Running && _bridge.LastSnapshot is { } snapshot)
        {
            Transport.ApplySnapshot(
                snapshot,
                snapshot.Recording?.Active == true,
                LiveProductionSync.IsStreamingLive(snapshot),
                ResolveProgramResolutionLabel(snapshot),
                MasterLimiterEnabled);
            ApplyConfiguredOutputReadouts(snapshot);
            NotifyShowReadinessChanged();
            return;
        }

        Transport.ApplyIdleState(Recording, Streaming, ProgramResolutionLabel);
        ApplyConfiguredOutputReadouts();
        NotifyShowReadinessChanged();
    }

    private void ApplyConfiguredOutputReadouts(NativeMediaCoreStateSnapshot? snapshot = null)
    {
        if (!Streaming &&
            (snapshot is null ||
             (!LiveProductionSync.IsStreamingLive(snapshot) &&
              LiveProductionSync.ResolveStreamHealthStatus(snapshot, streaming: false).Equals("idle", StringComparison.OrdinalIgnoreCase))))
        {
            Transport.StreamStatValue = FormatTransportStreamConfigLabel();
        }

        if (!Recording &&
            (snapshot is null ||
             (snapshot.Recording?.Active != true &&
              LiveProductionSync.ResolveRecordHealthStatus(snapshot, recording: false).Equals("idle", StringComparison.OrdinalIgnoreCase))))
        {
            Transport.RecordStatValue = FormatTransportRecordingConfigLabel();
        }
    }

    private string FormatTransportStreamConfigLabel() =>
        FormatTransportStreamConfigLabel(StreamTargetBitrateMbps);

    private string FormatTransportRecordingConfigLabel() =>
        FormatTransportRecordingConfigLabel(RecordingFormat, RecordingTargetBitrateMbps);

    private void NotifyShowReadinessChanged()
    {
        OnPropertyChanged(nameof(SetupProgressSummary));
        OnPropertyChanged(nameof(ShowReadinessSummary));
        OnPropertyChanged(nameof(ShowStatusSummary));
    }

    private void RefreshTransportAutomationState()
    {
        var canToggle = CanToggleSetAndForget() || ProductionMode == ProductionMode.SetAndForget;
        Transport.ApplyAutomationState(ProductionMode, canToggle);
    }

    private bool CanToggleSetAndForget()
    {
        var entitlements = LicenseCatalog.DeriveEntitlements(
            new LicenseState { Tier = Settings.LicenseTier, Status = Settings.LicenseStatus },
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        return entitlements.SetAndForget;
    }

    private static string ResolveProgramResolutionLabel(NativeMediaCoreStateSnapshot snapshot)
    {
        var profile = snapshot.OutputProfile;
        if (profile.Width > 0 && profile.Height > 0)
        {
            return TransportFormatting.ShortResolutionLabel($"{profile.Width}x{profile.Height}", profile.Fps);
        }

        return TransportFormatting.ShortResolutionLabel(profile.Resolution, profile.Fps);
    }

    public string SceneRailSummary =>
        $"{Scenes.Count} scenes · PGM {ProgramScene.Name} · PVW {PreviewScene.Name}";

    public string SceneRailDisplaySummary =>
        $"{VisibleScenes.Count} scenes - PGM {ProgramSceneSummary} - PVW {PreviewSceneSummary}";

    private IReadOnlyList<Scene> VisibleScenes =>
        Scenes
            .Where(scene => !IsInternalMultiviewSoloScene(scene.Id))
            .ToList();

    private void RefreshSceneItems()
    {
        SceneItems = VisibleScenes
            .Select((scene, index) => new SceneDisplayItem
            {
                Scene = scene,
                Id = scene.Id,
                SlotNumber = index + 1,
                IsOnProgram = scene.Id == ActiveSceneId,
                IsOnPreview = scene.Id == PreviewSceneId,
                CanRemove = CanRemoveScene(scene.Id),
                SelectCommand = SelectSceneCommand,
                RemoveCommand = RemoveSceneCommand,
                DuplicateCommand = DuplicateSceneCommand
            })
            .ToList();
        OnPropertyChanged(nameof(SceneItems));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
    }

    private static bool IsInternalMultiviewSoloScene(string sceneId) =>
        string.Equals(sceneId, MultiviewSoloSceneAId, StringComparison.Ordinal) ||
        string.Equals(sceneId, MultiviewSoloSceneBId, StringComparison.Ordinal);

    private bool CanRemoveScene(string sceneId) =>
        IsUserRemovableScene(sceneId) &&
        !string.Equals(sceneId, ActiveSceneId, StringComparison.Ordinal);

    private static bool IsUserRemovableScene(string sceneId) =>
        sceneId.StartsWith("custom-", StringComparison.Ordinal) ||
        sceneId.StartsWith("multiview-solo-", StringComparison.Ordinal);

    private string ResolveOnAirSceneLabel(string sceneId)
    {
        if (!IsInternalMultiviewSoloScene(sceneId))
        {
            return Scenes.FirstOrDefault(scene => scene.Id == sceneId)?.Name ?? "Unknown";
        }

        var route = GetMutableRoutes(sceneId).FirstOrDefault();
        if (route is null)
        {
            return "Manual one-up";
        }

        var sourceLabel = ResolveRouteSourceLabel(route);
        return string.IsNullOrWhiteSpace(sourceLabel)
            ? "Manual one-up"
            : $"Manual one-up: {sourceLabel}";
    }

    private string? ResolveRouteSourceLabel(SourceRoute route)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice)
        {
            return route.CaptureDeviceId is { Length: > 0 } deviceId
                ? CaptureDevices.FirstOrDefault(device =>
                    string.Equals(device.Id, deviceId, StringComparison.Ordinal)) is { } device
                    ? $"{device.Name} - {device.FormatLabel}"
                    : deviceId
                : "Capture input";
        }

        if (route.ParticipantId is { Length: > 0 } participantId)
        {
            return RoomVideoParticipants.FirstOrDefault(participant =>
                string.Equals(participant.Id, participantId, StringComparison.Ordinal))?.Name ?? participantId;
        }

        return route.Mode switch
        {
            SourceRouteMode.ActiveSpeaker => "Active speaker",
            SourceRouteMode.ScreenShare => "Screen share",
            SourceRouteMode.Spotlight => "Spotlight",
            _ => null
        };
    }

    private void InitializeSceneRoutes()
    {
        foreach (var scene in Scenes)
        {
            _sceneRoutes[scene.Id] = SceneRoutingService
                .GetRouteDefaults(scene, existingRoutes: null, RoomVideoParticipants)
                .Select(route => route.Clone())
                .ToList();
        }
    }

    private List<SourceRoute> GetMutableRoutes(string sceneId)
    {
        if (!_sceneRoutes.TryGetValue(sceneId, out var routes))
        {
            var scene = Scenes.First(item => item.Id == sceneId);
            routes = SceneRoutingService
                .GetRouteDefaults(scene, existingRoutes: null, RoomVideoParticipants)
                .Select(route => route.Clone())
                .ToList();
            _sceneRoutes[sceneId] = routes;
        }

        return routes;
    }

    // S2b (scenes redesign, pitfall A): when the cued PREVIEW scene is the same
    // scene that is LIVE on program, `_sceneRoutes[id]` is one shared list —
    // canvas edits previously mutated PROGRAM in real time. Editing paths now
    // go through this accessor: for a live scene it hands out a DRAFT copy, so
    // program stays untouched until "Update scene" commits it. The preview
    // sync context reads the draft too, so the preview bus (and the canvas)
    // show the edit as it is made while air stays stable.
    private List<SourceRoute>? _livePreviewDraft;
    private string? _livePreviewDraftSceneId;

    private List<SourceRoute> GetPreviewEditableRoutes()
    {
        if (!string.Equals(PreviewSceneId, ActiveSceneId, StringComparison.Ordinal))
        {
            DiscardLivePreviewDraft();
            return GetMutableRoutes(PreviewSceneId);
        }

        if (_livePreviewDraft is null ||
            !string.Equals(_livePreviewDraftSceneId, PreviewSceneId, StringComparison.Ordinal))
        {
            _livePreviewDraft = GetMutableRoutes(PreviewSceneId)
                .Select(route => route.Clone())
                .ToList();
            _livePreviewDraftSceneId = PreviewSceneId;
        }

        return _livePreviewDraft;
    }

    private void DiscardLivePreviewDraft()
    {
        _livePreviewDraft = null;
        _livePreviewDraftSceneId = null;
    }

    // Commits the live-scene draft into the stored scene (called by the Update
    // path after CopyPreviewRoutesToScene lands the routes).
    private void CommitLivePreviewDraft() => DiscardLivePreviewDraft();

    private void SchedulePreviewRoutingRefresh()
    {
        if (_previewRoutingRefreshScheduled)
        {
            return;
        }

        _previewRoutingRefreshScheduled = true;
        _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _previewRoutingRefreshScheduled = false;
            RefreshPreviewRoutingState();
        });
    }

    private void RefreshPreviewRoutingState()
    {
        RefreshSceneBackgroundSelection();
        RefreshSceneCompositionState(PreviewScene, PreviewSceneId, isPreview: true);
        RefreshSceneCompositionState(ProgramScene, ActiveSceneId, isPreview: false);
    }

    public void SetCanvasInteractionActive(bool isActive) =>
        _canvasInteractionActive = isActive;

    private void RefreshSceneCompositionState(Scene scene, string sceneId, bool isPreview)
    {
        // Preview may be editing a draft of the on-air scene. Refreshing from the
        // stored program routes here silently erased newly added overlay layers.
        var mutableRoutes = isPreview ? GetPreviewEditableRoutes() : GetMutableRoutes(sceneId);
        var defaults = SceneRoutingService.GetRouteDefaults(
            scene,
            mutableRoutes,
            RoomVideoParticipants);

        ReconcileRoutes(mutableRoutes, defaults);
        SceneCanvasLayoutService.EnsureCanvasRects(mutableRoutes, scene.Layout);
        var workingRoutes = mutableRoutes.Select(ResolveRouteFromShowInput).ToList();

        if (isPreview)
        {
            SyncPreviewCanvasLayers(mutableRoutes);
            PublishPreviewCompositionState(scene, workingRoutes);
        }
        else
        {
            var sceneTiles = BuildSceneTiles(scene, workingRoutes, isProgramScene: true);
            ProgramSceneRoutes = workingRoutes;
            ProgramSceneTiles = sceneTiles;
            OnPropertyChanged(nameof(ProgramSceneRoutes));
            OnPropertyChanged(nameof(ProgramSceneTiles));
            OnPropertyChanged(nameof(ProgramSceneBackgroundAsset));
            OnPropertyChanged(nameof(StudioLowerThirdCompactStatus));
            OnPropertyChanged(nameof(StudioLowerThirdSourceLabel));
            OnPropertyChanged(nameof(StudioLowerThirdToolTip));
            UpdateProgramLowerThirdKey(ResolveProgramLowerThirdSource(workingRoutes));
        }
    }

    // NOT force: forcing re-ran the full building-out -> building-in slide on EVERY
    // refresh, and this fires ~continuously (snapshot applies, active-speaker changes,
    // scene refreshes call it from ~9 sites). That made the lower third perpetually slide
    // down-and-up (owner: "goes up and down") AND cancelled each new source's transition
    // before it could settle (owner: "isn't auto-updating as new people go into program").
    // Un-forced: a same-source refresh updates position/text in place with no animation;
    // only a genuine source change animates once. (2026-07-11)
    public void RefreshProgramLowerThirdKeyPosition() =>
        UpdateProgramLowerThirdKey(ResolveProgramLowerThirdSource(ProgramSceneRoutes));

    private void UpdateProgramLowerThirdKey(LowerThirdSource? source, bool force = false)
    {
        var lowerThirdEnabled = ShouldEnableProgramLowerThird(
            ProgramLowerThirdEnabled,
            ProductionMode,
            AutomationLowerThirdsEnabled,
            _programLowerThirdAutomationSuppressed,
            Graphics.Any(graphic => graphic.Enabled && graphic.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase)));

        if (!lowerThirdEnabled || source is null)
        {
            // Keep the shell preview and compositor on the same out phase.
            // Jumping directly to hidden made the UI vanish while the native
            // graphic was still moving and let refreshes assert conflicting states.
            if (ProgramLowerThirdKey.IsVisible &&
                !string.Equals(ProgramLowerThirdKey.Phase, "building-out", StringComparison.Ordinal))
            {
                _lowerThirdKeyTransitionCts?.Cancel();
                _lowerThirdTargetSourceId = string.Empty;
                var outTransitionCts = new CancellationTokenSource();
                _lowerThirdKeyTransitionCts = outTransitionCts;
                _ = RunLowerThirdKeyOutAsync(outTransitionCts.Token);
            }
            return;
        }

        // Re-key (slide out/in) ONLY on a genuine source SWITCH, compared against the
        // TARGET we are already showing / animating toward -- NOT the currently-displayed
        // key. During a build-OUT the displayed key still holds the OLD source id while the
        // resolver already returns the NEW one, so comparing against the displayed key
        // restarted the transition on every sync tick = the "weird looping when you change
        // sources with LT on". Same target => no-op while a transition is mid-flight, or an
        // in-place text refresh once settled on-air. Never restarts the slide. (2026-07-11)
        if (!force && string.Equals(_lowerThirdTargetSourceId, source.SourceId, StringComparison.Ordinal))
        {
            if (ProgramLowerThirdKey.Enabled &&
                string.Equals(ProgramLowerThirdKey.Phase, "on-air", StringComparison.Ordinal))
            {
                var next = BuildLowerThirdKey(source, "on-air");
                if (ProgramLowerThirdKey != next)
                {
                    ProgramLowerThirdKey = next;
                    _ = TrySyncMediaCoreAsync();
                }
            }
            return;
        }

        // Debounce backstop: if the resolver still churns (co-program route order flicker),
        // never restart the slide more than ~once/sec. The sync storm from rapid re-keys --
        // each fires a full media-core sync batch -- is what caused the operator latency.
        // First show (no target yet) is never debounced, so a genuine show is instant.
        var nowMs = Environment.TickCount64;
        if (!force && _lowerThirdTargetSourceId.Length > 0 &&
            nowMs - _lowerThirdKeyChangeTickMs < 1200)
        {
            return;
        }
        _lowerThirdKeyChangeTickMs = nowMs;

        LaunchLog.Write($"lower-third: key -> '{source.SourceId}' (name '{source.SourceName}')");
        _lowerThirdTargetSourceId = source.SourceId;
        _lowerThirdKeyTransitionCts?.Cancel();
        var transitionCts = new CancellationTokenSource();
        _lowerThirdKeyTransitionCts = transitionCts;
        _ = RunLowerThirdKeyTransitionAsync(source, ProgramLowerThirdKey.IsVisible, transitionCts.Token);
    }

    private async Task RunLowerThirdKeyTransitionAsync(LowerThirdSource source, bool hasCurrentKey, CancellationToken cancellationToken)
    {
        try
        {
            if (hasCurrentKey)
            {
                ProgramLowerThirdKey = ProgramLowerThirdKey with { Phase = "building-out" };
                if (!await SyncLowerThirdPhaseAsync(cancellationToken).ConfigureAwait(true))
                {
                    return;
                }
                await WaitForLowerThirdPhaseAsync(
                    () => NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs),
                    cancellationToken).ConfigureAwait(true);
            }

            ProgramLowerThirdKey = BuildLowerThirdKey(source, "building-in");
            LowerThirdName = source.SourceName;
            LowerThirdTitle = source.Title;
            LowerThirdOrg = source.Org;
            if (!await SyncLowerThirdPhaseAsync(cancellationToken).ConfigureAwait(true))
            {
                return;
            }
            await WaitForLowerThirdPhaseAsync(
                () => NormalizeLowerThirdTimingMs(LowerThirdBuildInMs),
                cancellationToken).ConfigureAwait(true);
            ProgramLowerThirdKey = BuildLowerThirdKey(source, "on-air");
            _ = TrySyncMediaCoreAsync();
        }
        catch (OperationCanceledException)
        {
        }
    }

    private async Task RunLowerThirdKeyOutAsync(CancellationToken cancellationToken)
    {
        try
        {
            ProgramLowerThirdKey = ProgramLowerThirdKey with
            {
                Phase = "building-out",
                BuildOutMs = NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs)
            };
            if (!await SyncLowerThirdPhaseAsync(cancellationToken).ConfigureAwait(true))
            {
                return;
            }
            await WaitForLowerThirdPhaseAsync(
                () => NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs),
                cancellationToken).ConfigureAwait(true);
            ProgramLowerThirdKey = LowerThirdKeyState.Hidden(Overlays.LowerThirdPosition);
            _ = TrySyncMediaCoreAsync();
        }
        catch (OperationCanceledException)
        {
        }
    }

    private async Task<bool> SyncLowerThirdPhaseAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (_applyingProductionPatch)
            {
                await Task.Delay(25, cancellationToken).ConfigureAwait(true);
            }

            await RetryLowerThirdPhaseSyncAsync(
                async () =>
                {
                    await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(false);
                    await SyncActiveSceneAsync().ConfigureAwait(false);
                },
                cancellationToken).ConfigureAwait(true);
            return true;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            CommandStatus = ex.Message;
            return false;
        }
    }

    public static async Task RetryLowerThirdPhaseSyncAsync(
        Func<Task> syncAttempt,
        CancellationToken cancellationToken,
        int retryDelayMs = 25)
    {
        ArgumentNullException.ThrowIfNull(syncAttempt);
        var delayMs = Math.Max(1, retryDelayMs);
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                await syncAttempt().ConfigureAwait(false);
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                await Task.Delay(delayMs, cancellationToken).ConfigureAwait(false);
            }
        }
    }

    private static async Task WaitForLowerThirdPhaseAsync(Func<int> currentDurationMs, CancellationToken cancellationToken)
    {
        var stopwatch = Stopwatch.StartNew();
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var remainingMs = RemainingLowerThirdPhaseDelayMs(stopwatch.ElapsedMilliseconds, currentDurationMs());
            if (remainingMs <= 0)
            {
                return;
            }

            await Task.Delay(Math.Min(remainingMs, 50), cancellationToken).ConfigureAwait(true);
        }
    }

    private LowerThirdKeyState BuildLowerThirdKey(LowerThirdSource source, string phase) =>
        new()
        {
            SourceId = source.SourceId,
            SourceName = source.SourceName,
            Title = source.Title,
            Org = source.Org,
            Position = Overlays.LowerThirdPosition,
            Phase = phase,
            BuildInMs = NormalizeLowerThirdTimingMs(LowerThirdBuildInMs),
            BuildOutMs = NormalizeLowerThirdTimingMs(LowerThirdBuildOutMs),
            Enabled = true
        };

    public static int NormalizeLowerThirdTimingMs(double value) =>
        (int)Math.Round(
            double.IsFinite(value) ? Math.Clamp(value, 50, 2000) : 250,
            MidpointRounding.AwayFromZero);

    public static int RemainingLowerThirdPhaseDelayMs(long elapsedMs, double currentDurationMs) =>
        Math.Max(0, NormalizeLowerThirdTimingMs(currentDurationMs) - ClampElapsedToInt(elapsedMs));

    private static int ClampElapsedToInt(long value) =>
        value <= int.MinValue ? int.MinValue :
        value >= int.MaxValue ? int.MaxValue :
        (int)value;

    public static bool ShouldEnableProgramLowerThird(
        bool manuallyEnabled,
        ProductionMode productionMode,
        bool automationLowerThirdsEnabled,
        bool automationSuppressed,
        bool graphicLowerThirdEnabled) =>
        manuallyEnabled ||
        graphicLowerThirdEnabled ||
        (productionMode == ProductionMode.SetAndForget &&
         automationLowerThirdsEnabled &&
         !automationSuppressed);

    public static string FormatStudioLowerThirdActionLabel(bool isVisible) =>
        isVisible ? "LT out" : "LT in";

    public static string FormatLowerThirdPhaseLabel(string? phase) =>
        phase?.Trim().ToLowerInvariant() switch
        {
            "building-in" => "Building in",
            "building-out" => "Building out",
            "on-air" => "On",
            _ => "On"
        };

    private LowerThirdSource? ResolveProgramLowerThirdSource(IReadOnlyList<SourceRoute> workingRoutes)
    {
        var sources = workingRoutes
            .OrderBy(route => route.ZIndex)
            .Select(ResolveLowerThirdSource)
            .Where(source => source is not null)
            .Cast<LowerThirdSource>()
            .ToList();

        if (sources.Count == 0)
        {
            return null;
        }

        // Stickiness: keep the source the lower third is ALREADY showing as long as it is
        // still on program. Without this the choice ping-ponged between two co-program
        // sources whose route order flickers (active-speaker re-sorting upstream re-orders
        // ZIndex every ~1s), which changed the target and restarted the slide every tick =
        // the residual loop. A genuine change (the current source leaves program) still
        // re-picks below, so it keeps following manual cuts. (2026-07-11)
        var current = sources.FirstOrDefault(source =>
            string.Equals(source.SourceId, _lowerThirdTargetSourceId, StringComparison.Ordinal));
        if (current is not null)
        {
            return current;
        }

        // No sticky source (first show, or the current one left program): pick
        // DETERMINISTICALLY, ordered by SourceId -- NOT the ZIndex route order, which the
        // active-speaker/fake-engine churn re-sorts every tick. A stable tiebreak means two
        // co-program sources can't alternate the choice, so it can't ping-pong.
        return sources.Where(source => !source.IsScreenShare)
                      .OrderBy(source => source.SourceId, StringComparer.Ordinal)
                      .FirstOrDefault()
               ?? sources.OrderBy(source => source.SourceId, StringComparer.Ordinal).First();
    }

    private LowerThirdSource? ResolveLowerThirdSource(SourceRoute route)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice && route.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            var device = CaptureDevices.FirstOrDefault(item =>
                string.Equals(item.Id, captureDeviceId, StringComparison.Ordinal));
            if (device is null)
            {
                return null;
            }

            var captureSourceId = ShowInputRosterService.CaptureSourceId(device.Id);
            return new LowerThirdSource(
                captureSourceId,
                ResolveSourceDisplayName(captureSourceId, device.Name),
                // A camera source has no presenter role/org -- leave the secondary lines
                // blank so the lower third shows just the (operator-assignable) name
                // instead of the device kind ("UVC", from device.Vendor) and resolution.
                string.Empty,
                string.Empty,
                IsActiveSpeaker: false,
                IsScreenShare: false);
        }

        var participant = SceneRoutingService.ResolveRouteParticipant(route, RoomVideoParticipants);
        if (participant is null)
        {
            return null;
        }

        var isScreenShare = route.Mode == SourceRouteMode.ScreenShare || participant.IsScreenSharing;
        var title = isScreenShare
            ? "Screen share"
            : string.IsNullOrWhiteSpace(participant.Title)
                ? participant.RoleLabel
                : participant.Title;

        return new LowerThirdSource(
            participant.Id,
            ResolveSourceDisplayName(ShowInputRosterService.ZoomSourceId(participant.Id), participant.Name),
            title,
            string.IsNullOrWhiteSpace(participant.BreakoutRoomName)
                ? participant.RoleLabel
                : participant.BreakoutRoomName,
            participant.IsActiveSpeaker,
            isScreenShare);
    }

    private static void ReconcileRoutes(List<SourceRoute> mutableRoutes, IReadOnlyList<SourceRoute> defaults)
    {
        while (mutableRoutes.Count > defaults.Count)
        {
            mutableRoutes.RemoveAt(mutableRoutes.Count - 1);
        }

        while (mutableRoutes.Count < defaults.Count)
        {
            mutableRoutes.Add(defaults[mutableRoutes.Count].Clone());
        }

        for (var index = 0; index < defaults.Count; index++)
        {
            SceneRoutingService.ApplyRouteValues(mutableRoutes[index], defaults[index]);
        }
    }

    private void SyncPreviewCanvasLayers(IReadOnlyList<SourceRoute> routes)
    {
        if (_canvasInteractionActive)
        {
            return;
        }

        if (PreviewCanvasLayers.Count == routes.Count)
        {
            for (var index = 0; index < routes.Count; index++)
            {
                PreviewCanvasLayers[index].SyncFromRoute(RoomVideoParticipants, CaptureDevices, ShowInputs, VisualMediaAssets);
                PreviewCanvasLayers[index].SetSurface(ResolveLayerSurface(routes[index], index));
            }

            OnPropertyChanged(nameof(PreviewCanvasLayers));
            return;
        }

        PreviewCanvasLayers.Clear();
        for (var index = 0; index < routes.Count; index++)
        {
            PreviewCanvasLayers.Add(new SceneCanvasLayerViewModel(
                index,
                routes[index],
                RoomVideoParticipants,
                CaptureDevices,
                ShowInputs,
                VisualMediaAssets,
                OnPreviewCanvasLayerChanged));
            PreviewCanvasLayers[^1].SetSurface(ResolveLayerSurface(routes[index], index));
        }
    }

    private void PublishPreviewCompositionState(Scene scene, IReadOnlyList<SourceRoute> workingRoutes)
    {
        PreviewSceneParticipants = SceneRoutingService.DescribeRouteAssignments(
            scene,
            workingRoutes,
            RoomVideoParticipants);

        PreviewRouteWarnings = SceneRoutingService
            .GetRouteWarnings(scene, workingRoutes, RoomVideoParticipants)
            .Take(3)
            .ToList();

        PreviewSceneRoutes = workingRoutes;
        PreviewSceneTiles = BuildSceneTiles(scene, workingRoutes, isProgramScene: false);
        OnPropertyChanged(nameof(PreviewRouteWarnings));
        OnPropertyChanged(nameof(HasPreviewRouteWarnings));
        OnPropertyChanged(nameof(PreviewSceneTiles));
        OnPropertyChanged(nameof(PreviewSceneRoutes));
        OnPropertyChanged(nameof(PreviewSceneBackgroundAsset));
        OnPropertyChanged(nameof(PreviewSceneParticipants));
        OnPropertyChanged(nameof(PreviewSlotCount));
        OnPropertyChanged(nameof(HasPreviewSlotEditors));
        OnPropertyChanged(nameof(SceneBuilderSlotSummary));
    }

    private IReadOnlyList<ParticipantSurfaceTile> BuildSceneTiles(
        Scene scene,
        IReadOnlyList<SourceRoute> routes,
        bool isProgramScene)
    {
        var tilesByParticipant = MultiviewTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        var sceneTiles = new List<ParticipantSurfaceTile>();
        foreach (var route in routes)
        {
            if (ResolveRouteTile(route, tilesByParticipant, isProgramScene) is { } routeTile)
            {
                sceneTiles.Add(routeTile);
            }
        }

        if (sceneTiles.Count > 0)
        {
            return sceneTiles;
        }

        var participants = SceneRoutingService.GetSceneParticipants(scene, routes, RoomVideoParticipants);
        return participants
            .Select(participant => BuildParticipantSceneTile(participant, tilesByParticipant))
            .ToList();
    }

    private VideoSurfaceState ResolveLayerSurface(SourceRoute route, int index)
    {
        var resolved = ResolveRouteFromShowInput(route);
        var tilesByParticipant = MultiviewTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        if (ResolveRouteTile(resolved, tilesByParticipant, isProgramScene: false) is { } tile)
        {
            return tile.Surface with
            {
                SurfaceKey = $"scene-layer-{index + 1}:{tile.Surface.SurfaceKey}",
                Kind = VideoSurfaceKind.Multiview,
                Title = $"{index + 1}. {tile.Participant.Name}"
            };
        }

        return VideoSurfaceState.Waiting(
            VideoSurfaceKind.Multiview,
            $"scene-layer-{index + 1}",
            $"Source {index + 1}") with
            {
                DetailLine = resolved.Mode switch
                {
                    SourceRouteMode.ScreenShare => "Screen share will appear when available.",
                    SourceRouteMode.ActiveSpeaker => "Active speaker will appear during a Zoom meeting.",
                    SourceRouteMode.None => "This source is parked.",
                    _ => "Choose a live Show Input or connected capture device."
                }
            };
    }

    private ParticipantSurfaceTile? ResolveRouteTile(
        SourceRoute route,
        IReadOnlyDictionary<string, ParticipantSurfaceTile> tilesByParticipant,
        bool isProgramScene)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice)
        {
            return BuildCaptureSceneTile(route.CaptureDeviceId);
        }

        if (route.Mode == SourceRouteMode.Fixed &&
            ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var mediaAssetId))
        {
            return BuildMediaSceneTile(mediaAssetId, isProgramScene);
        }

        var participant = SceneRoutingService.ResolveRouteParticipant(route, RoomVideoParticipants);
        return participant is null ? null : BuildParticipantSceneTile(participant, tilesByParticipant);
    }

    private ParticipantSurfaceTile? BuildMediaSceneTile(string mediaAssetId, bool isProgramScene)
    {
        if (FindMediaAsset(mediaAssetId) is not { } asset)
        {
            return null;
        }

        var playback = MediaRoutePlaybackService.ResolveSceneRoutePlayback(
            asset.Id,
            isProgramScene,
            SelectedMediaAssetId,
            SelectedMediaAssetPlaying,
            GetResolvedProgramRoutes(),
            _programMediaPlaybackTakeVersion);
        var mediaSourceId = ShowInputRosterService.ToMediaSourceId(asset.Id);
        var surfaceKey = isProgramScene ? $"program:{mediaSourceId}" : $"preview:{mediaSourceId}";
        return new ParticipantSurfaceTile
        {
            Participant = new Participant
            {
                Id = mediaSourceId,
                Name = asset.Name,
                Title = asset.Kind,
                Role = ParticipantRole.Guest,
                Health = playback.Playing ? FeedHealth.Live : FeedHealth.VideoOff
            },
            Surface = VideoSurfaceState.MediaAssetPreview(
                surfaceKey,
                asset.Name,
                asset.FilePath,
                asset.Kind,
                playback.Playing,
                playback.MediaPlaybackKey,
                asset.NaturalWidth,
                asset.NaturalHeight)
        };
    }

    private SourceRoute ResolveRouteFromShowInput(SourceRoute route)
    {
        var resolved = route.Clone();

        // R1: role-targeted route — resolve to whichever participant currently
        // holds the production role. No holder (or holder left) -> ParticipantId
        // stays null and the layer renders the placeholder slate, exactly like
        // a fixed route to an absent participant.
        if (!string.IsNullOrWhiteSpace(resolved.ProductionRoleId))
        {
            resolved.ParticipantId = _participantProductionRoles
                .FirstOrDefault(pair => string.Equals(pair.Value, resolved.ProductionRoleId, StringComparison.OrdinalIgnoreCase))
                .Key;
            ApplyKnownColorGradeToRoute(resolved);
            return resolved;
        }

        // A freshly launched standalone studio has no Zoom role holders. Keep the
        // default Speaker + Slides scene immediately testable by resolving its transient
        // role routes to assigned local camera/screen inputs. The stored scene remains
        // role-based and takes over normally once Zoom video participants are present.
        if (ShowInputRosterService.TryApplyStandaloneRoleFallback(
                resolved,
                ShowInputs,
                RoomVideoParticipants))
        {
            ApplyKnownColorGradeToRoute(resolved);
            return resolved;
        }

        if (route.ShowInputSlotNumber is not { } slotNumber ||
            ShowInputs.FirstOrDefault(slot => slot.SlotNumber == slotNumber) is not { } slot ||
            !slot.IsAssigned)
        {
            return resolved;
        }

        // Pure slot -> route mapping lives in the roster service so it stays unit-testable.
        // Media slots resolve to a Fixed route carrying a "media:{assetId}" identity in
        // ParticipantId, matching how ad-hoc media is added to the multiview
        // (see RefreshMultiviewGridTiles / BuildMediaSceneTile).
        ShowInputRosterService.ApplySlotRoute(resolved, slot);
        ApplyKnownColorGradeToRoute(resolved);
        return resolved;
    }

    private void ApplyKnownColorGradeToRoute(SourceRoute route)
    {
        var sourceId = ResolveColorGradeSourceId(route);
        if (sourceId is not null && _sourceColorGrades.TryGetValue(sourceId, out var grade))
        {
            route.ColorGrade = grade;
        }
    }

    private static ParticipantSurfaceTile BuildParticipantSceneTile(
        Participant participant,
        IReadOnlyDictionary<string, ParticipantSurfaceTile> tilesByParticipant) =>
        tilesByParticipant.TryGetValue(participant.Id, out var tile)
            ? tile
            : new ParticipantSurfaceTile
            {
                Participant = participant,
                Surface = VideoSurfaceState.Waiting(
                    VideoSurfaceKind.Multiview,
                    $"participant:{participant.Id}",
                    participant.Name)
            };

    private ParticipantSurfaceTile? BuildCaptureSceneTile(string? captureDeviceId)
    {
        if (string.IsNullOrWhiteSpace(captureDeviceId) ||
            CaptureDevices.FirstOrDefault(device => string.Equals(device.Id, captureDeviceId, StringComparison.Ordinal)) is not { } device)
        {
            return null;
        }

        var label = $"{device.Name} - {device.ResolutionLabel}";
        var captureSurfaces = _surfaces.CaptureDeviceSurfaces;
        var hasLiveSurface = captureSurfaces.TryGetValue(device.Id, out var liveSurface) &&
            liveSurface.HasPreviewBitmap;
        var surface = hasLiveSurface
            ? liveSurface! with
            {
                SurfaceKey = $"capture:{device.Id}",
                Kind = VideoSurfaceKind.Multiview,
                Title = label
            }
            : (device.IsConnected
                ? VideoSurfaceState.CaptureSourceOnline(VideoSurfaceKind.Multiview, $"capture:{device.Id}", label)
                : VideoSurfaceState.Waiting(VideoSurfaceKind.Multiview, $"capture:{device.Id}", label)) with
                {
                    DetailLine = device.IsConnected
                        ? $"{device.ConnectionLabel} - {device.SignalLabel} - waiting for capture frames"
                        : "Connect device in Sources to bring online."
                };

        return new ParticipantSurfaceTile
        {
            Participant = new Participant
            {
                Id = $"capture:{device.Id}",
                Name = label,
                Title = device.Vendor,
                Role = ParticipantRole.Guest,
                Health = hasLiveSurface ? FeedHealth.Live : device.IsConnected ? FeedHealth.Live : FeedHealth.VideoOff
            },
            Surface = surface,
            SourceIndex = 1
        };
    }

    [RelayCommand]
    private void AddCanvasSource(string? optionValue)
    {
        if (string.IsNullOrWhiteSpace(optionValue))
        {
            return;
        }

        if (string.Equals(optionValue, "media", StringComparison.OrdinalIgnoreCase) &&
            string.IsNullOrWhiteSpace(SelectedMediaAssetId))
        {
            CommandStatus = "Select a media asset before adding Media to the scene";
            return;
        }

        var routes = GetPreviewEditableRoutes();
        var index = routes.Count;
        var route = SceneRoutingService.BuildAddedSourceRoute(PreviewSceneId, index, optionValue, SelectedMediaAssetId);
        SceneRoutingService.ApplyNormalizeRouteUpdate(route, RoomVideoParticipants);
        routes.Add(route);

        var label = string.Equals(optionValue, "media", StringComparison.OrdinalIgnoreCase) &&
            SelectedMediaAssetName is { Length: > 0 } mediaName
                ? mediaName
                : AddSourceOptions.FirstOrDefault(option => option.Value == optionValue)?.Label ?? optionValue;
        CommandStatus = $"{label} added to {PreviewScene.Name}";

        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    // POS-2 (sources-redesign-spec §B2): add a small top-most "bug" overlay layer to
    // the PREVIEW scene. Parameter is "<placement>|<option>" from the Add-overlay
    // flyouts (Studio preview header + Scenes tab). CRITICAL: this goes through
    // GetPreviewEditableRoutes — the S2b draft path — so a scene that is live on
    // PROGRAM is untouched until Take/Update, exactly like every other canvas edit.
    [RelayCommand]
    private void AddOverlayLayer(string? parameter)
    {
        if (!OverlayLayerService.TryParseAddOverlayParameter(
                parameter, out var placement, out var optionValue, out var mediaAssetId))
        {
            CommandStatus = $"Add overlay failed: malformed request '{parameter}'";
            return;
        }

        double? sourceAspect = null;
        string sourceLabel;
        if (mediaAssetId is not null)
        {
            if (FindMediaAsset(mediaAssetId) is not { } asset)
            {
                CommandStatus = "Add overlay failed: that media asset no longer exists - re-import it on the Media tab";
                return;
            }

            sourceAspect = OverlayLayerService.AspectFromNaturalSize(asset.NaturalWidth, asset.NaturalHeight);
            sourceLabel = asset.Name;
        }
        else
        {
            sourceLabel = optionValue.StartsWith("capture:", StringComparison.OrdinalIgnoreCase)
                ? CaptureDevices.FirstOrDefault(device =>
                    string.Equals(device.Id, optionValue["capture:".Length..], StringComparison.Ordinal))?.Name ?? optionValue
                : AddSourceOptions.FirstOrDefault(option => option.Value == optionValue)?.Label ?? optionValue;
        }

        var routes = GetPreviewEditableRoutes();
        OverlayLayerService.AppendOverlayRoute(
            routes,
            PreviewSceneId,
            optionValue,
            mediaAssetId,
            sourceAspect,
            placement,
            PreviewScene.Layout,
            RoomVideoParticipants);

        LaunchLog.Write(
            $"overlay: added parameter='{parameter}' scene={PreviewSceneId} routes={routes.Count} " +
            $"source={optionValue} placement={placement}");

        CommandStatus =
            $"{sourceLabel} overlay added to {PreviewScene.Name} ({OverlayLayerService.PlacementLabel(placement)}) - drag or use Edit layout to fine-tune";
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    [RelayCommand]
    private void AddBrowserOverlay(string? browserId)
    {
        if (string.IsNullOrWhiteSpace(browserId) ||
            !BrowserCaptureDevices.Any(device => string.Equals(device.Id, browserId, StringComparison.Ordinal)))
        {
            CommandStatus = "Add browser overlay failed: browser source is no longer available";
            return;
        }

        AddOverlayLayer($"full-canvas|capture:{browserId}");
    }

    [RelayCommand]
    private void ToggleBrowserOverlayOnAir(string? browserId)
    {
        if (string.IsNullOrWhiteSpace(browserId) ||
            BrowserCaptureDevices.FirstOrDefault(device =>
                string.Equals(device.Id, browserId, StringComparison.Ordinal)) is not { } browser)
        {
            CommandStatus = "Browser overlay is no longer available";
            return;
        }

        _primaryBrowserOverlayId = browser.Id;
        var showing = !_onAirBrowserOverlayIds.Contains(browser.Id);
        _onAirBrowserOverlayIds.Clear();
        if (showing)
        {
            _onAirBrowserOverlayIds.Add(browser.Id);
        }

        foreach (var device in BrowserCaptureDevices)
        {
            device.IsBrowserOverlayOnAir = _onAirBrowserOverlayIds.Contains(device.Id);
        }
        NotifyBrowserOverlayControlStateChanged();
        CommandStatus = $"{browser.Name} overlay {(showing ? "shown on" : "hidden from")} Program";
        LaunchLog.Write($"overlay: browser key {(showing ? "in" : "out")} id={browser.Id}");

        SyncBrowserOverlayKeyChange("browser-overlay-program-key");
    }

    [RelayCommand]
    private void ToggleBrowserOverlayPreview(string? browserId)
    {
        if (string.IsNullOrWhiteSpace(browserId) ||
            BrowserCaptureDevices.FirstOrDefault(device =>
                string.Equals(device.Id, browserId, StringComparison.Ordinal)) is not { } browser)
        {
            CommandStatus = "Browser overlay is no longer available";
            return;
        }

        _primaryBrowserOverlayId = browser.Id;
        var staging = !_previewBrowserOverlayIds.Contains(browser.Id);
        _previewBrowserOverlayIds.Clear();
        if (staging)
        {
            _previewBrowserOverlayIds.Add(browser.Id);
        }

        foreach (var device in BrowserCaptureDevices)
        {
            device.IsBrowserOverlayInPreview = _previewBrowserOverlayIds.Contains(device.Id);
        }
        NotifyBrowserOverlayControlStateChanged();
        CommandStatus = $"{browser.Name} overlay {(staging ? "staged on" : "cleared from")} Preview";
        LaunchLog.Write($"overlay: browser DSK preview {(staging ? "on" : "off")} id={browser.Id}");

        SyncBrowserOverlayKeyChange("browser-overlay-preview-key");
    }

    private void SyncBrowserOverlayKeyChange(string reason)
    {
        if (_applyingProductionPatch)
        {
            QueueProductionSyncRetry(reason);
        }
        else
        {
            _ = TrySyncMediaCoreAsync();
        }
    }

    [RelayCommand]
    private void TogglePrimaryBrowserOverlayPreview()
    {
        if (PrimaryBrowserOverlay is { } browser)
        {
            ToggleBrowserOverlayPreview(browser.Id);
        }
        else
        {
            CommandStatus = "Add a browser overlay URL first";
        }
    }

    [RelayCommand]
    private void TogglePrimaryBrowserOverlay()
    {
        if (PrimaryBrowserOverlay is { } browser)
        {
            ToggleBrowserOverlayOnAir(browser.Id);
        }
        else
        {
            CommandStatus = "Add a browser overlay URL first";
        }
    }

    private void NotifyBrowserOverlayControlStateChanged()
    {
        OnPropertyChanged(nameof(StudioBrowserOverlayButtonLabel));
        OnPropertyChanged(nameof(StudioBrowserOverlayPreviewButtonLabel));
        OnPropertyChanged(nameof(StudioBrowserOverlayPreviewStatus));
        OnPropertyChanged(nameof(StudioBrowserOverlayStatus));
        OnPropertyChanged(nameof(StudioBrowserOverlayToolTip));
    }

    [RelayCommand]
    private async Task ReloadBrowserOverlayAsync(string? browserId)
    {
        if (!string.IsNullOrWhiteSpace(browserId))
        {
            await ReloadBrowserSourceAsync(browserId);
        }
    }

    [RelayCommand]
    private async Task AddBrowserOverlayFromUrlAsync()
    {
        var originalUrl = NewBrowserSourceUrl;
        NewBrowserSourceUrl = NormalizeBrowserOverlayUrl(originalUrl);
        if (!string.Equals(originalUrl, NewBrowserSourceUrl, StringComparison.Ordinal))
        {
            LaunchLog.Write("overlay: removed forced browser background from overlay URL");
        }

        var existingIds = BrowserCaptureDevices
            .Select(device => device.Id)
            .ToHashSet(StringComparer.Ordinal);

        await AddBrowserSourceAsync().ConfigureAwait(true);

        CaptureDevice? added = null;
        for (var attempt = 0; attempt < 20 && added is null; attempt++)
        {
            added = BrowserCaptureDevices.LastOrDefault(device => !existingIds.Contains(device.Id));
            if (added is not null)
            {
                break;
            }

            await Task.Delay(100).ConfigureAwait(true);
            await RefreshCaptureDevicesAsync().ConfigureAwait(true);
        }

        if (added is null)
        {
            if (!CommandStatus.Contains("FAILED", StringComparison.OrdinalIgnoreCase) &&
                !CommandStatus.StartsWith("Enter a URL", StringComparison.OrdinalIgnoreCase))
            {
                CommandStatus = "Browser source started, but CoreVideo could not attach its Preview layer.";
            }
            LaunchLog.Write("overlay: browser source catalog timeout; Preview layer not attached");
            return;
        }

        _primaryBrowserOverlayId = added.Id;
        _previewBrowserOverlayIds.Clear();
        _previewBrowserOverlayIds.Add(added.Id);
        foreach (var device in BrowserCaptureDevices)
        {
            device.IsBrowserOverlayInPreview = _previewBrowserOverlayIds.Contains(device.Id);
        }
        NotifyBrowserOverlayControlStateChanged();
        SyncBrowserOverlayKeyChange("browser-overlay-added-to-preview");
        CommandStatus = $"{added.Name} staged on DSK Preview - use DSK in when ready";
    }

    public static string NormalizeBrowserOverlayUrl(string? value)
    {
        var url = value?.Trim() ?? string.Empty;
        if (!Uri.TryCreate(url, UriKind.Absolute, out var parsed) || string.IsNullOrEmpty(parsed.Query))
        {
            return url;
        }

        var kept = parsed.Query.TrimStart('?')
            .Split('&', StringSplitOptions.RemoveEmptyEntries)
            .Where(part =>
            {
                var separator = part.IndexOf('=');
                var key = separator >= 0 ? part[..separator] : part;
                return !string.Equals(Uri.UnescapeDataString(key), "bg", StringComparison.OrdinalIgnoreCase);
            })
            .ToArray();

        if (kept.Length == parsed.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries).Length)
        {
            return url;
        }

        var builder = new UriBuilder(parsed)
        {
            Query = string.Join('&', kept)
        };
        return builder.Uri.AbsoluteUri;
    }

    // Scenes redesign S1: the missing layer primitives. Removing a source and
    // reordering layers were previously IMPOSSIBLE — add was append-only and
    // z-order was hardwired to add-order.
    [RelayCommand]
    private void RemoveCanvasSource(SceneCanvasLayerViewModel? layer)
    {
        var routes = GetPreviewEditableRoutes();
        if (layer is null || layer.LayerIndex < 0 || layer.LayerIndex >= routes.Count)
        {
            return;
        }

        routes.RemoveAt(layer.LayerIndex);
        ReindexRouteZOrder(routes);
        CommandStatus = $"{layer.LayerLabel} removed from {PreviewScene.Name}";
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    [RelayCommand]
    private void MoveCanvasSourceForward(SceneCanvasLayerViewModel? layer) => MoveCanvasSource(layer, +1);

    [RelayCommand]
    private void MoveCanvasSourceBack(SceneCanvasLayerViewModel? layer) => MoveCanvasSource(layer, -1);

    private void MoveCanvasSource(SceneCanvasLayerViewModel? layer, int delta)
    {
        var routes = GetPreviewEditableRoutes();
        if (layer is null)
        {
            return;
        }

        var from = layer.LayerIndex;
        var to = from + delta;
        if (from < 0 || from >= routes.Count || to < 0 || to >= routes.Count)
        {
            return;
        }

        (routes[from], routes[to]) = (routes[to], routes[from]);
        ReindexRouteZOrder(routes);
        CommandStatus = $"{layer.LayerLabel} moved {(delta > 0 ? "forward" : "back")} in {PreviewScene.Name}";
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    // List order IS layer order: zIndex mirrors the index (later = drawn on
    // top), so a list swap is a real stacking change through the wire.
    private static void ReindexRouteZOrder(List<SourceRoute> routes)
    {
        for (var index = 0; index < routes.Count; index++)
        {
            routes[index].ZIndex = index;
        }
    }

    [RelayCommand]
    private void AddSelectedMediaAssetToPreview()
    {
        if (string.IsNullOrWhiteSpace(SelectedMediaAssetId) ||
            FindMediaAsset(SelectedMediaAssetId) is not { } asset)
        {
            CommandStatus = "Select a media asset before adding it to Preview";
            return;
        }

        AddCanvasSourceCommand.Execute("media");
        MediaPlaybackStatus = $"{asset.Name} cued in Preview; Take the scene to Program to auto-play";
        CommandStatus = MediaPlaybackStatus;
        OnPropertyChanged(nameof(MediaPlaybackStatus));
    }

    public void ApplyCanvasPreset(string presetWire)
    {
        var routes = GetPreviewEditableRoutes();
        // Non-destructive presets (scenes redesign S2): the old flow wiped every
        // hand-placed rect AND re-pointed every route's source assignment
        // (ApplyInputSlotTemplate). Now: snapshot for one-step undo, keep
        // existing assignments (trim extras / append unassigned slots only for
        // the count difference), and apply GEOMETRY (rects + stacking; framing
        // resets because the boxes moved, but fit/borders/opacity survive).
        _canvasPresetUndo = (PreviewSceneId, routes.Select(route => route.Clone()).ToList());
        CanUndoCanvasPreset = true;
        UpdateSceneLayout(PreviewSceneId, presetWire);
        if (SceneCanvasLayoutService.TemplateSlotCount(presetWire) is { } slotCount)
        {
            while (routes.Count > slotCount)
            {
                routes.RemoveAt(routes.Count - 1);
            }

            while (routes.Count < slotCount)
            {
                routes.Add(SceneRoutingService.BuildAddedSourceRoute(
                    PreviewSceneId, routes.Count, $"input-{routes.Count + 1}"));
            }
        }

        SceneCanvasLayoutService.ApplyPresetGeometry(presetWire, routes);
        CommandStatus = $"{PreviewScene.Name} preset applied ({presetWire}) - sources kept; Undo preset reverts";
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    [ObservableProperty]
    private bool _canUndoCanvasPreset;

    private (string SceneId, List<SourceRoute> Routes)? _canvasPresetUndo;

    [RelayCommand]
    private void UndoCanvasPreset()
    {
        if (_canvasPresetUndo is not { } undo ||
            !string.Equals(undo.SceneId, PreviewSceneId, StringComparison.Ordinal))
        {
            CanUndoCanvasPreset = false;
            return;
        }

        var routes = GetPreviewEditableRoutes();
        routes.Clear();
        routes.AddRange(undo.Routes.Select(route => route.Clone()));
        _canvasPresetUndo = null;
        CanUndoCanvasPreset = false;
        CommandStatus = $"Preset undone in {PreviewScene.Name}";
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    private void UpdateSceneLayout(string sceneId, string layout)
    {
        var index = _scenes.ToList().FindIndex(scene => string.Equals(scene.Id, sceneId, StringComparison.Ordinal));
        if (index < 0 || string.Equals(_scenes[index].Layout, layout, StringComparison.Ordinal))
        {
            return;
        }

        var scene = _scenes[index];
        _scenes[index] = new Scene
        {
            Id = scene.Id,
            Name = scene.Name,
            Layout = layout,
            Automation = scene.Automation,
            DurationLabel = scene.DurationLabel
        };

        OnPropertyChanged(nameof(ProgramScene));
        OnPropertyChanged(nameof(PreviewScene));
        OnPropertyChanged(nameof(ProgramSceneSummary));
        OnPropertyChanged(nameof(PreviewSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
    }

    public void CommitPreviewCanvasLayer(SceneCanvasLayerViewModel layer) =>
        OnPreviewCanvasLayerChanged(layer);

    private void OnPreviewCanvasLayerChanged(SceneCanvasLayerViewModel layer)
    {
        var routes = GetPreviewEditableRoutes();
        if (layer.LayerIndex < 0 || layer.LayerIndex >= routes.Count)
        {
            return;
        }

        layer.ApplyRoute();
        SceneRoutingService.ApplyNormalizeRouteUpdate(routes[layer.LayerIndex], RoomVideoParticipants);
        ApplyKnownColorGradeToRoute(routes[layer.LayerIndex]);
        layer.SyncFromRoute(RoomVideoParticipants, CaptureDevices, ShowInputs, VisualMediaAssets);
        layer.SetSurface(ResolveLayerSurface(routes[layer.LayerIndex], layer.LayerIndex));

        CommandStatus = $"{PreviewScene.Name} source {layer.LayerIndex + 1} updated on canvas";
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        OnPropertyChanged(nameof(PreviewCanvasLayers));
        SyncLiveSceneEditIfNeeded(PreviewSceneId);
    }

    private void SyncLiveSceneEditIfNeeded(string sceneId)
    {
        // Every edit to the queued PREVIEW scene must reach set-preview-scene, even
        // when that scene differs from PROGRAM. The old program-only gate updated
        // WinUI's route list but never sent browser/bug layers to the native preview
        // compositor, so "Show in Preview" appeared to do nothing in normal PGM/PVW
        // operation. Keep the program check for callers that edit the live scene.
        if (SceneBackgroundSelectionService.SceneEditNeedsMediaCoreSync(
                sceneId,
                PreviewSceneId,
                ActiveSceneId))
        {
            // Explicit operator edits must not disappear when they land during a
            // snapshot patch. TrySyncMediaCoreAsync intentionally ignores calls made
            // while applying a patch to prevent feedback loops, so queue one bounded
            // retry here; the edited route remains in the draft and the retry sends it.
            if (_applyingProductionPatch)
            {
                QueueProductionSyncRetry("preview-scene-edit");
                return;
            }

            _ = TrySyncMediaCoreAsync();
        }
    }

    private void CopyPreviewRoutesToScene(string sceneId)
    {
        var previewRoutes = GetPreviewEditableRoutes()
            .Select(route => route.Clone())
            .ToList();
        GetMutableRoutes(sceneId).Clear();
        GetMutableRoutes(sceneId).AddRange(previewRoutes);
        if (string.Equals(sceneId, _livePreviewDraftSceneId, StringComparison.Ordinal))
        {
            // Draft committed into the stored scene (Update path): retire it so
            // the accessor re-clones from the now-identical stored routes.
            CommitLivePreviewDraft();
        }
    }

    public void HandleAppActivation(AppActivationArguments args)
    {
        _zoomOAuthCoordinator.HandleActivationArguments(args);
        _zoomOAuthCoordinator.TryDrainPendingCallback();
        if (args.Kind == ExtendedActivationKind.Protocol)
        {
            SelectTab("settings");
        }
    }

    public Task ForceStopMediaCoreAsync()
    {
        ForceShutdownMediaCore();
        return _bridge.DisposeAsync().AsTask();
    }

    public async ValueTask DisposeAsync()
    {
        LaunchLog.Write("shutdown: disposing studio view model");
        _automationTimer.Stop();
        _bridge.HealthChanged -= OnBridgeHealthChanged;
        _bridge.StatusChanged -= OnBridgeStatusChanged;
        _bridge.ProfileChanged -= OnBridgeProfileChanged;
        _bridge.SnapshotChanged -= OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived -= OnZoomVideoFrameReceived;
        _bridge.ProgramFramePreviewReceived -= OnProgramFramePreviewReceived;
        _bridge.ProgramSharedTextureReceived -= OnProgramSharedTextureReceived;
        _bridge.PreviewSharedTextureReceived -= OnPreviewSharedTextureReceived;
        _bridge.ParticipantSharedTextureReceived -= OnParticipantSharedTextureReceived;
        _bridge.MultiviewSharedTextureReceived -= OnMultiviewSharedTextureReceived;
        CaptureDeviceFrameRouter.FrameReceived -= OnCaptureDeviceFrameReceived;
        _surfaces.SurfacesChanged -= OnSurfacesChanged;
        SrtIngestSources.CollectionChanged -= OnSrtIngestSourcesChanged;
        foreach (var source in SrtIngestSources)
        {
            source.PropertyChanged -= OnSrtIngestSourcePropertyChanged;
        }

        ForceShutdownMediaCore();

        _surfaces.Dispose();
        _captureFrameReader.Dispose();
        _captureDiscovery.Dispose();
        _audioCaptureDiscovery.Dispose();
        _audioRenderDiscovery.Dispose();
        _zoomOAuthCoordinator.Dispose();
        await _bridge.DisposeAsync().ConfigureAwait(false);
        LaunchLog.Write("shutdown: studio view model disposed");
    }

    private void ForceShutdownMediaCore()
    {
        try
        {
            _bridge.ConfigureZoomSpineSync(null);
            _surfaces.SetZoomCaptureSubscribed(false);
            if (_bridge.Running)
            {
                LaunchLog.Write("shutdown: stopping media core");
                _bridge.Stop();
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"shutdown: media core stop failed ({ex.Message})");
            try
            {
                _bridge.Stop();
            }
            catch
            {
                // Best effort.
            }
        }
    }

    private void OnZoomVideoFrameReceived(ZoomVideoFrame frame)
    {
        _surfaces.OnZoomVideoFrame(frame);
        Settings.ObserveZoomVideoFrame(frame);
    }

    private void OnCaptureDeviceFrameReceived(CaptureDeviceFrame frame)
    {
        // Bridge every capture frame into the native core via shared memory (full
        // rate, independent of the throttled UI preview update) so the core
        // composites real capture pixels into the program frame / recording / stream.
        var shm = CaptureDeviceSharedMemoryWriter.Write(frame.DeviceId, frame.Bgra, frame.Width, frame.Height);
        if (shm.MappingChanged && !string.IsNullOrEmpty(shm.ShmName))
        {
            _ = _bridge.RegisterCaptureShmAsync(frame.DeviceId, shm.ShmName, shm.Width, shm.Height);
        }

        if (_surfaces.OnCaptureDeviceFrame(frame))
        {
            RunOnUiThread(() => ApplyCaptureDeviceFrameTelemetry(frame));
        }
    }

    private void ApplyCaptureDeviceFrameTelemetry(CaptureDeviceFrame frame)
    {
        if (CaptureDevices.FirstOrDefault(device => string.Equals(device.Id, frame.DeviceId, StringComparison.Ordinal)) is { } device)
        {
            device.ApplyFrameTelemetry(frame.Width, frame.Height, frame.Fps);
            RefreshCaptureFleetSummary();
        }
    }

    private void OnProgramFramePreviewReceived(ProgramFramePreview preview)
    {
        _surfaces.OnProgramFramePreview(preview);
        Settings.ObserveProgramPreviewFrame(preview);
    }

    // The shared-texture handle drives the GPU program present (SwapChainPanel + D3D),
    // which are UI-thread/COM-affinitized. Marshal handling to the UI thread so the
    // surface update and the present happen on the same thread as every other UI
    // access — touching them from the core read-loop thread throws RPC_E_WRONG_THREAD.
    private long _sharedTextureReceiveCount;
    private long _sharedTextureRateStampMs;

    private void OnProgramSharedTextureReceived(ProgramSharedTexture texture)
    {
        // Diagnostic: how fast the core actually delivers new program frames (the
        // content render rate). The vsync present re-copies the texture at 60fps
        // regardless, so this — not the present count — is the real frame rate.
        if (++_sharedTextureReceiveCount % 120 == 0)
        {
            var nowMs = Environment.TickCount64;
            var dt = nowMs - _sharedTextureRateStampMs;
            LaunchLog.Write($"sharedtex: received #{_sharedTextureReceiveCount} ~{(dt > 0 ? 120000.0 / dt : 0):F1}/s (core content rate)");
            _sharedTextureRateStampMs = nowMs;
        }
        RunOnUiThread(() => _surfaces.OnProgramSharedTexture(texture));
    }

    // The core-composited PREVIEW bus shared texture. Marshal to the UI thread before
    // touching the preview VideoSurfaceHost surface state (off-thread sets on the bound
    // PreviewSurface are a known CoreMessagingXP fail-fast). The coordinator dedups
    // structurally, and RefreshSurfaceBindings picks the composite over the single-source
    // path — so bind PreviewSurface here too once the composite state settles.
    private int _previewSharedTextureReceiveCount;
    private void OnPreviewSharedTextureReceived(ProgramSharedTexture texture)
    {
        if (++_previewSharedTextureReceiveCount % 120 == 0)
        {
            LaunchLog.Write($"preview-sharedtex: received #{_previewSharedTextureReceiveCount} (core preview composite)");
        }
        RunOnUiThread(() =>
        {
            _surfaces.OnPreviewSharedTexture(texture);
            // Bind the preview monitor to the composite (or revert to single-source when retired).
            PreviewSurface = _surfaces.HasPreviewComposite
                ? _surfaces.PreviewCompositeSurface
                : ResolvePreviewPrimarySurface();
        });
    }

    // Per-participant GPU texture handle for the multiview tiles. Marshal to the UI
    // thread (touches the SwapChainPanel/surface state for the tile's VideoSurfaceHost).
    private readonly HashSet<string> _loggedParticipantTextures = new(StringComparer.Ordinal);
    private void OnParticipantSharedTextureReceived(ParticipantSharedTexture texture)
    {
        // DIAG (multiview bring-up): log the first GPU shared-texture handle seen per
        // participant so we can confirm the core->bridge->coordinator path end-to-end.
        if (texture is not null && texture.IsValid && _loggedParticipantTextures.Add(texture.ParticipantId))
        {
            LaunchLog.Write($"multiview: participant GPU texture '{texture.ParticipantId}' {texture.Width}x{texture.Height} handle={texture.SharedHandleHex}");
        }
        RunOnUiThread(() => _surfaces.OnParticipantSharedTexture(texture));
    }

    // The ONE core-composited multiview GPU shared texture. Low-rate (structural-change only).
    // Marshal to the UI thread (the coordinator fires SurfacesChanged, which feeds the single
    // multiview VideoSurfaceHost + click overlay).
    // GPU core-composited multiview is the intended default. Set COREVIDEO_MULTIVIEW_GPU=0/off/false
    // to suppress the set-multiview-layout command + GPU surface handling (the multiview monitor
    // then stays a slate — a debug escape hatch, not a CPU fallback).
    private static readonly bool MultiviewGpuEnabled = ResolveMultiviewGpuEnabled();

    private static bool ResolveMultiviewGpuEnabled()
    {
        var value = Environment.GetEnvironmentVariable("COREVIDEO_MULTIVIEW_GPU");
        if (string.IsNullOrWhiteSpace(value))
        {
            return true;
        }

        return !(value.Equals("0", StringComparison.Ordinal) ||
                 value.Equals("false", StringComparison.OrdinalIgnoreCase) ||
                 value.Equals("off", StringComparison.OrdinalIgnoreCase));
    }

    // The multiview layout is now delivered on the frequent, reliable zoom-media-spine-sync
    // channel (see BuildSpinePayload → ZoomMediaSpinePayloadBuilder; the core applies + dedups it
    // in MediaCore::syncZoomMediaSpine). The production sync's set-multiview-layout command stays
    // as a belt-and-suspenders. The old debounced re-entrant-sync machinery (which backpressured
    // against the spine flood and never reliably landed) is retired; this is now a no-op kept so
    // the structural roster/show-input refresh points have a harmless hook.
    private string _lastSentMultiviewLayoutSignature = "";
    private NativeMediaCoreCommand? _pendingMultiviewLayoutCommand;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _multiviewLayoutTimer;

    // Deliver the multiview layout whenever it STRUCTURALLY changes, independent of Zoom.
    // The spine sync also carries it, but ONLY while in a Zoom meeting (MeetingState==in_meeting)
    // — so capture-only shows (UVC/Blackmagic/AJA, no Zoom) had no delivery path once the
    // production-sync loop was removed. This sends a standalone set-multiview-layout (now that the
    // JSON \uXXXX parse fix lets it reach the core) on a debounced, signature-gated trigger: fires
    // only when the source set/slots/grid change (NOT on active-speaker churn — that's baked into
    // the texture in-core), so it neither loops nor floods.
    private void SyncMultiviewLayoutIfChanged()
    {
        if (!MultiviewGpuEnabled || !_bridge.Running)
        {
            return;
        }

        var sources = ShowInputRosterService.BuildMultiviewLayoutSources(
            ShowInputs, RoomParticipantsForInputs, CaptureDevices, VisualMediaAssets, _sourceDisplayNames);
        var grid = ShowInputRosterService.ResolveGridShape(sources.Count);
        var signature = string.Join("|", sources.Select(s => $"{s.Slot}:{s.Kind}:{s.SourceId}:{s.Label}")) +
            $"#{grid.Columns}x{grid.Rows}";
        if (string.Equals(signature, _lastSentMultiviewLayoutSignature, StringComparison.Ordinal))
        {
            return;
        }

        _lastSentMultiviewLayoutSignature = signature;
        var canvas = BuildRequestedOutputProfile("canvas", CanvasResolution, CanvasFps, "h264");
        _pendingMultiviewLayoutCommand = MediaCoreCommandBuilder.BuildMultiviewLayoutCommand(
            sources, canvas.Width, canvas.Height, grid.Columns, grid.Rows);

        if (_multiviewLayoutTimer is null)
        {
            _multiviewLayoutTimer = _dispatcher.CreateTimer();
            _multiviewLayoutTimer.IsRepeating = false;
            _multiviewLayoutTimer.Tick += (_, _) => _ = SendPendingMultiviewLayoutAsync();
        }
        _multiviewLayoutTimer.Interval = TimeSpan.FromMilliseconds(150);  // coalesce rapid changes
        _multiviewLayoutTimer.Start();
    }

    private async Task SendPendingMultiviewLayoutAsync()
    {
        var command = _pendingMultiviewLayoutCommand;
        if (command is null)
        {
            return;
        }

        try
        {
            // Standalone sync — does NOT apply the response snapshot, so it can't re-trigger the
            // production-sync path (no loop).
            await _bridge.SyncAsync([command]).ConfigureAwait(false);
        }
        catch
        {
            // Backpressure/transient — drop the cached signature so the next structural change retries.
            _lastSentMultiviewLayoutSignature = "";
        }
    }

    private bool _loggedMultiviewTexture;
    private void OnMultiviewSharedTextureReceived(CoreVideoPro.MediaCore.Models.MultiviewSharedTexture multiview)
    {
        if (!MultiviewGpuEnabled)
        {
            return;
        }

        if (multiview is { } mv && mv.IsValid && !_loggedMultiviewTexture)
        {
            _loggedMultiviewTexture = true;
            LaunchLog.Write($"multiview: GPU shared texture {mv.Texture.Width}x{mv.Texture.Height} handle={mv.Texture.SharedHandleHex} tiles={mv.Tiles.Count}");
        }
        RunOnUiThread(() => _surfaces.OnMultiviewSharedTexture(multiview));
    }

    private sealed record LowerThirdSource(
        string SourceId,
        string SourceName,
        string Title,
        string Org,
        bool IsActiveSpeaker,
        bool IsScreenShare);
}
