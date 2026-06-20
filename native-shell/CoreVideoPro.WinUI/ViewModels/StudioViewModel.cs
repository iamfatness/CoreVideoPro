using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
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
    private ProductionSettingsWindow? _productionSettingsWindow;
    private CancellationTokenSource? _lowerThirdKeyTransitionCts;
    private readonly HashSet<ColorGradeEditorViewModel> _openColorGradeEditors = [];
    private IReadOnlyList<AudioCaptureDevice> _lastDiscoveredAudioCaptureDevices = [];
    private readonly Dictionary<string, List<string>> _audioProcessingInserts = new(StringComparer.Ordinal);

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
    private string? _selectedParticipantId;

    [ObservableProperty]
    private bool _recording;

    [ObservableProperty]
    private bool _streaming;

    [ObservableProperty]
    private bool _masterLimiterEnabled = true;

    [ObservableProperty]
    private bool _audioMonitoringEnabled;

    [ObservableProperty]
    private string _selectedAudioMonitorDeviceId = string.Empty;

    [ObservableProperty]
    private double _audioMonitorVolume = 0.75;

    [ObservableProperty]
    private bool _localAudioSourceEnabled;

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
    private string _recordingRenderResolution = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.Resolution;

    [ObservableProperty]
    private string _recordingRenderFps = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.Fps.ToString();

    [ObservableProperty]
    private string _recordingVideoCodec = MediaCoreProductionSyncContext.DefaultRecordingOutputProfile.Codec;

    [ObservableProperty]
    private string _recordingTargetFolder = MediaCoreProductionSyncContext.DefaultRecordingTargets.TargetFolder;

    [ObservableProperty]
    private string _recordingFilenamePrefix = MediaCoreProductionSyncContext.DefaultRecordingTargets.FilenamePrefix;

    [ObservableProperty]
    private string _recordingFormat = MediaCoreProductionSyncContext.DefaultRecordingTargets.Format;

    [ObservableProperty]
    private string _recordingQuality = MediaCoreProductionSyncContext.DefaultRecordingTargets.Quality;

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
    private VideoSurfaceState _programSurface = VideoSurfaceState.Slate(VideoSurfaceKind.Program, "program", "Program");

    [ObservableProperty]
    private VideoSurfaceState _previewSurface = VideoSurfaceState.Slate(VideoSurfaceKind.Preview, "preview", "Preview");

    [ObservableProperty]
    private IReadOnlyList<ParticipantSurfaceTile> _multiviewTiles = [];

    [ObservableProperty]
    private IReadOnlyList<ParticipantSurfaceTile> _multiviewGridTiles = [];

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

    [ObservableProperty]
    private bool _automationCaptionsEnabled = true;

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
    // Per-source color grades keyed by participant id or capture:<deviceId>.
    private readonly Dictionary<string, ColorGrade> _sourceColorGrades = new(StringComparer.Ordinal);
    private string? _automationPendingSceneId;
    private DateTimeOffset? _automationPendingSince;
    private bool _automationTakeInFlight;
    private bool _previewRoutingRefreshScheduled;
    private bool _showInputRefreshScheduled;
    private bool _multiviewGridRefreshScheduled;
    private bool _canvasInteractionActive;
    private bool _applyingDualCaptureSelection;
    private readonly HashSet<string> _captureAutoConnectInFlight = new(StringComparer.Ordinal);

    public SettingsViewModel Settings { get; }

    public TransportViewModel Transport { get; }

    public OverlaysViewModel Overlays { get; }

    public ObservableCollection<GraphicOverlay> Graphics { get; } = [];

    public ObservableCollection<CaptureDevice> CaptureDevices { get; } = [];

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
        _ = TrySyncMediaCoreAsync();
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

    public IReadOnlyList<AudioParticipantRow> AudioParticipantRows { get; private set; } = [];

    public IReadOnlyList<AudioProcessingTargetOption> AudioProcessingTargetOptions { get; private set; } = [];

    public string CaptionQualitySummary =>
        ProductionStateHelper.CaptionQualitySummary(
            !string.IsNullOrWhiteSpace(CaptionText) || CaptionTranscript.Count > 0,
            CaptionTranscript.Count);

    public ObservableCollection<SceneCanvasLayerViewModel> PreviewCanvasLayers { get; } = [];

    public AudioRoutingMatrixViewModel AudioRoutingMatrix { get; } = new();

    public VideoRoutingMatrixViewModel VideoRoutingMatrix { get; } = new();

    /// <summary>Which matrix the Routing tab is showing: "audio" or "video".</summary>
    [ObservableProperty]
    private string _routingMatrixMode = "audio";

    public bool IsAudioRoutingMode => RoutingMatrixMode == "audio";

    public bool IsVideoRoutingMode => RoutingMatrixMode == "video";

    public TabChrome AudioRoutingModeChrome =>
        IsAudioRoutingMode ? SelectedViewModeChrome : DefaultViewModeChrome;

    public TabChrome VideoRoutingModeChrome =>
        IsVideoRoutingMode ? SelectedViewModeChrome : DefaultViewModeChrome;

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

    public string MasterLimiterSummary =>
        MasterLimiterEnabled
            ? "Master protection is armed; activity only lights when peaks reach the ceiling."
            : "Master protection is bypassed; no limiting is applied.";

    public string SelectedAudioMonitorDeviceName =>
        AudioRenderDevices.FirstOrDefault(device =>
            string.Equals(device.Id, SelectedAudioMonitorDeviceId, StringComparison.Ordinal))?.Name ??
        "No monitor output selected";

    public string AudioMonitorVolumeLabel => $"{AudioMonitorVolume * 100:0}%";

    public string AudioMonitorStatus =>
        AudioMonitoringEnabled
            ? $"Monitor target - {SelectedAudioMonitorDeviceName}"
            : "Monitor muted";

    public string AudioMonitorEngineStatus =>
        _bridge.LastSnapshot?.AudioMixSession is { } audio
            ? audio.MasterLevel <= 0 && audio.MixedFrameCount <= 0
                ? "No mixed audio frames from the media engine."
                : audio.MonitorEnabled
                    ? $"{audio.MasterLevel}% master - {audio.MixedFrameCount} mixed frames - monitor {FormatMonitorStatus(audio)}"
                    : $"{audio.MasterLevel}% master - {audio.MixedFrameCount} mixed frames - monitor muted"
            : "Waiting for media engine audio telemetry.";

    public string SelectedLocalAudioCaptureDeviceName =>
        AudioCaptureDevices.FirstOrDefault(device =>
            string.Equals(device.Id, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal))?.DisplayLabel ??
        "No local audio input selected";

    public string LocalAudioSourceStatus =>
        LocalAudioSourceEnabled
            ? $"Local source routed - {SelectedLocalAudioCaptureDeviceName}"
            : "Local machine audio source disabled";

    public string FfmpegRuntimeStatus
    {
        get
        {
            if (string.IsNullOrWhiteSpace(FfmpegBinDirectory))
            {
                return "FFmpeg not configured. RTMP/RTMPS runtime packaging needs a bin folder containing avformat*.dll.";
            }

            return Directory.Exists(FfmpegBinDirectory) &&
                   Directory.EnumerateFiles(FfmpegBinDirectory, "avformat*.dll").Any()
                ? "FFmpeg runtime found. Package scripts can stage RTMP/RTMPS DLLs from this folder."
                : "Folder does not contain avformat*.dll. Choose the FFmpeg bin folder, not the install root.";
        }
    }

    public StudioViewModel()
    {
        ExternalUriLauncher.BindDispatcher(Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());
        AudioRoutingMatrix.RouteChanged += OnAudioRoutingMatrixChanged;
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
            onMeetingPresenceChanged: () =>
            {
                OnPropertyChanged(nameof(CanToggleCapture));
                OnPropertyChanged(nameof(CanToggleRecording));
                OnPropertyChanged(nameof(CaptureEngineHint));
                ToggleEngineCommand.NotifyCanExecuteChanged();
                ToggleRecordingCommand.NotifyCanExecuteChanged();
            },
            onBeforeLeaveMeeting: () =>
            {
                if (ZoomCaptureSubscribed)
                {
                    UnsubscribeZoomCapture("Capture off — leaving meeting");
                }

                return Task.CompletedTask;
            },
            zoomStatusChanged: status => ZoomStatus = status,
            onMeetingJoined: () => ActiveTab = StudioTab.Studio);
        _zoomOAuthCoordinator.SetStatusChangedHandler(message =>
        {
            Settings.OauthStatusMessage = message;
            _ = Settings.RefreshOAuthStatusAsync();
        });
        _zoomOAuthCoordinator.TryDrainPendingCallback();
        Transport = new TransportViewModel();
        Overlays = new OverlaysViewModel(this);
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
        _bridge.SnapshotChanged += OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived += OnZoomVideoFrameReceived;
        _bridge.ProgramFramePreviewReceived += OnProgramFramePreviewReceived;
        _bridge.ProgramSharedTextureReceived += _surfaces.OnProgramSharedTexture;
        CaptureDeviceFrameRouter.FrameReceived += OnCaptureDeviceFrameReceived;
        _surfaces.SurfacesChanged += OnSurfacesChanged;

        MediaBinGuidance = MediaBinClassifier.BuildEmptyGuidanceMessage();
        _captureDiscovery.StartWatching(() => _ = RefreshCaptureDevicesAsync());
        _audioCaptureDiscovery.StartWatching(() => _ = RefreshAudioCaptureDevicesAsync());
        _audioRenderDiscovery.StartWatching(() => _ = RefreshAudioRenderDevicesAsync());
        _ = RefreshCaptureDevicesAsync();
        _ = RefreshAudioCaptureDevicesAsync();
        _ = RefreshAudioRenderDevicesAsync();
        _ = StartMediaCoreOnLaunchAsync();
    }

    private readonly ObservableCollection<Scene> _scenes;

    public IReadOnlyList<Scene> Scenes => _scenes;

    public IReadOnlyList<SceneDisplayItem> SceneItems { get; private set; } = [];

    public IReadOnlyList<Participant> RoomVideoParticipants { get; private set; }

    public IReadOnlyList<Participant> SceneParticipants => RoomVideoParticipants;

    public Scene ProgramScene => Scenes.First(s => s.Id == ActiveSceneId);

    public Scene PreviewScene => Scenes.First(s => s.Id == PreviewSceneId);

    [ObservableProperty]
    private string _currentRoomLabel;

    public string EngineRunningLabel => ZoomCaptureSubscribed ? "Capture On" : "Capture Off";

    public bool CanToggleCapture => Settings.IsInMeeting;

    // Recording rights can be requested in a breakout room without capture running,
    // so recording only requires being in a meeting (NOT an active capture subscription).
    public bool CanToggleRecording => Settings.IsInMeeting;

    public string CaptureEngineHint => CanToggleCapture
        ? "Requests the raw Zoom capture subscription for the active meeting."
        : "Join a Zoom meeting first, then request capture.";

    public string RecordingLabel => Recording ? "Recording" : "Record";

    public string StreamingLabel => Streaming ? "Streaming" : "Stream";

    public IReadOnlyList<string> StreamRtmpProtocolOptions { get; } = ["rtmps", "rtmp"];

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
        "Each SRT source is exposed as a routable input. Real SRT receive/decode requires the libsrt ingest adapter.";

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

    public string CanvasProfileSummary =>
        $"{CanvasResolution} - {NormalizeFpsText(CanvasFps)} fps canvas";

    public string StreamRenderProfileSummary =>
        $"{StreamRenderResolution} - {NormalizeFpsText(StreamRenderFps)} fps - {FormatVideoCodec(StreamVideoCodec)}";

    public string RecordingRenderProfileSummary =>
        $"{RecordingRenderResolution} - {NormalizeFpsText(RecordingRenderFps)} fps - {FormatVideoCodec(RecordingVideoCodec)}";

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
            ? IsNdiConfigured()
                ? $"NDI ready - {NormalizeOutputText(StreamNdiProgramName, string.Empty)}"
                : "NDI needs a program name"
            : "NDI disabled";

    public string StreamSrtSummary =>
        StreamSrtEnabled
            ? ValidateSrtSettings() is null
                ? $"SRT ready - {StudioStreamOutputValidation.NormalizeSrtMode(StreamSrtMode)} {NormalizeOutputText(StreamSrtHost, string.Empty)}:{NormalizeOutputText(StreamSrtPort, string.Empty)} - {NormalizeOutputText(StreamSrtLatencyMs, "120")} ms ({BuildSrtEncryptionSummary()})"
                : "SRT needs mode, host, port, latency, and valid encryption settings"
            : "SRT disabled";

    public string RecordingOptionsSummary =>
        $"{RecordingFormat.ToUpperInvariant()} · {RecordingQuality} · {RecordingFilenamePrefix}";

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

    public string VstBridgeStatusLabel =>
        SelectedAudioMix?.PluginInserts.Any(insert => insert.StartsWith("VST", StringComparison.OrdinalIgnoreCase)) == true
            ? "VST3 bridge slot configured - scan/load bridge required for live PCM processing"
            : "No VST bridge slot on selected channel";

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

    public string ProcessingBridgeStatusLabel =>
        SelectedAudioProcessingInsertLabel.Contains("VST", StringComparison.OrdinalIgnoreCase)
            ? "VST3 slot is staged for this processing target; live plugin execution still requires the native plugin bridge."
            : "Built-in processing settings are synced with the native media core metadata.";

    public Brush EngineToggleBackground => ZoomCaptureSubscribed
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(31, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(10, 255, 255, 255));

    public Brush EngineToggleBorder => ZoomCaptureSubscribed
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(140, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(46, 189, 207, 196));

    public Brush EngineToggleForeground => ZoomCaptureSubscribed
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(255, 174, 242, 223))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(255, 148, 165, 155));

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
            var active = ShowInputRosterService.CountActiveShowInputs(ShowInputs);
            var displayed = MultiviewGridTiles.Count(tile => !tile.IsEmpty);
            return $"Show inputs · {displayed} of {active} live · {ShowInputRosterService.MaxShowInputs}-input capacity";
        }
    }

    public string ShowInputSummary =>
        $"{ShowInputs.Count(slot => slot.InShow)}/{ShowInputRosterService.MaxMultiviewBoxes} in show · " +
        $"{ShowInputRosterService.MaxShowInputs} slots — pick input type + source, then toggle In show";

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

    partial void OnRoutingMatrixModeChanged(string value)
    {
        OnPropertyChanged(nameof(IsAudioRoutingMode));
        OnPropertyChanged(nameof(IsVideoRoutingMode));
        OnPropertyChanged(nameof(AudioRoutingModeChrome));
        OnPropertyChanged(nameof(VideoRoutingModeChrome));
    }

    partial void OnMultiviewTilesChanged(IReadOnlyList<ParticipantSurfaceTile> value)
    {
        OnPropertyChanged(nameof(MultiviewHeader));
    }

    partial void OnMultiviewGridTilesChanged(IReadOnlyList<ParticipantSurfaceTile> value)
    {
        OnPropertyChanged(nameof(MultiviewHeader));
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
        if (_bridge.LastSnapshot is { } snapshot)
        {
            ApplyMeetingFieldsFromSnapshot(snapshot);
        }

        OnPropertyChanged(nameof(CurrentRoomHeader));
        RefreshTransportState();
    }

    partial void OnRecordingChanged(bool value)
    {
        OnPropertyChanged(nameof(RecordingLabel));
        OnPropertyChanged(nameof(RecordButtonBackground));
        OnPropertyChanged(nameof(RecordButtonBorder));
        OnPropertyChanged(nameof(RecordButtonForeground));
        OnPropertyChanged(nameof(RecordingLiveDotVisibility));
        OnPropertyChanged(nameof(RecordIconVisibility));
        RefreshTransportState();
    }

    partial void OnStreamingChanged(bool value)
    {
        OnPropertyChanged(nameof(StreamingLabel));
        OnPropertyChanged(nameof(StreamButtonBackground));
        OnPropertyChanged(nameof(StreamButtonBorder));
        OnPropertyChanged(nameof(StreamButtonForeground));
        RefreshTransportState();
    }

    partial void OnMasterLimiterEnabledChanged(bool value)
    {
        RefreshAudioReadoutBindings();
        RefreshTransportState();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnAudioMonitoringEnabledChanged(bool value) => OnAudioMonitorSettingsChanged();

    partial void OnSelectedAudioMonitorDeviceIdChanged(string value) => OnAudioMonitorSettingsChanged();

    partial void OnAudioMonitorVolumeChanged(double value) => OnAudioMonitorSettingsChanged();

    partial void OnLocalAudioSourceEnabledChanged(bool value)
    {
        RefreshLocalAudioSourceBindings();
        _ = TrySyncMediaCoreAsync();
    }

    partial void OnSelectedLocalAudioCaptureDeviceIdChanged(string value)
    {
        RefreshLocalAudioSourceBindings();
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
    }

    partial void OnFfmpegBinDirectoryChanged(string value) =>
        OnPropertyChanged(nameof(FfmpegRuntimeStatus));

    partial void OnStreamRtmpEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamNdiEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamRtmpProtocolChanged(string value) => OnStreamOutputOptionChanged();

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

    partial void OnCanvasResolutionChanged(string value) => OnOutputProfileChanged();

    partial void OnCanvasFpsChanged(string value) => OnOutputProfileChanged();

    partial void OnStreamRenderResolutionChanged(string value) => OnOutputProfileChanged();

    partial void OnStreamRenderFpsChanged(string value) => OnOutputProfileChanged();

    partial void OnStreamVideoCodecChanged(string value) => OnOutputProfileChanged();

    partial void OnRecordingRenderResolutionChanged(string value) => OnOutputProfileChanged();

    partial void OnRecordingRenderFpsChanged(string value) => OnOutputProfileChanged();

    partial void OnRecordingVideoCodecChanged(string value) => OnOutputProfileChanged();

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
        RefreshSceneItems();
        OnPropertyChanged(nameof(PreviewScene));
        OnPropertyChanged(nameof(PreviewSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
        OnPropertyChanged(nameof(CanTake));
        TakeCommand.NotifyCanExecuteChanged();
        SchedulePreviewRoutingRefresh();
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
        RefreshAudioParticipantRows();
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
    }

    partial void OnAutomationAutoTakeEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationPreferScreenShareChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationLowerThirdsEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationCaptionsEnabledChanged(bool value) => OnAutomationPolicyChanged();

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

    [RelayCommand]
    private void ResetAutomationDefaults()
    {
        AutomationAutoTakeEnabled = true;
        AutomationPreferScreenShare = true;
        AutomationLowerThirdsEnabled = true;
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
                UnsubscribeZoomCapture("Capture off — raw Zoom ingest paused");
            }
            else
            {
                // The media core should already be running from launch. If it died, bring it back up rather than dead-ending.
                await EnsureMediaCoreRunningAsync("Restarting media core...").ConfigureAwait(false);

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
                await SyncActiveSceneAsync().ConfigureAwait(false);
                RefreshSurfaceBindings();
                RefreshTransportState();
                ToggleRecordingCommand.NotifyCanExecuteChanged();
            }
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

        PreviewSceneId = scene.Id;
        OnPropertyChanged(nameof(PreviewSceneSummary));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
        CommandStatus = $"{tile.Participant.Name} queued as manual one-up preview";
        SchedulePreviewRoutingRefresh();
    }

    public bool CanTake => PreviewSceneId != ActiveSceneId;

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
    }

    [RelayCommand]
    private void SaveScene(string? name)
    {
        var trimmed = string.IsNullOrWhiteSpace(name) ? null : name.Trim();
        var existing = _scenes.FirstOrDefault(
            s => trimmed is not null && string.Equals(s.Name, trimmed, StringComparison.OrdinalIgnoreCase));

        if (existing is not null)
        {
            // A scene with this name already exists — overwrite its routes with the current canvas.
            CopyPreviewRoutesToScene(existing.Id);
            PreviewSceneId = existing.Id;
            CommandStatus = $"{existing.Name} updated from canvas";
            SchedulePreviewRoutingRefresh();
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
        _sceneRoutes[newId] = GetMutableRoutes(PreviewSceneId)
            .Select(route => route.Clone())
            .ToList();

        RefreshSceneItems();
        PreviewSceneId = newId;
        CommandStatus = $"{scene.Name} saved from canvas";
        SchedulePreviewRoutingRefresh();
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
        RefreshPreviewRoutingState();
        CommandStatus = $"{ProgramSceneSummary} taken with fade";
        OutputStatus = "Program updated";

        if (_bridge.Running)
        {
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                CommandStatus = ex.Message;
            }
        }
    }

    [RelayCommand(CanExecute = nameof(CanToggleRecording))]
    private async Task ToggleRecordingAsync()
    {
        Recording = !Recording;
        RefreshOutputStatus();

        try
        {
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            OutputStatus = ex.Message;
        }
    }

    [RelayCommand]
    private async Task ToggleStreamingAsync()
    {
        if (!Streaming && BuildSelectedStreamDestinations().Count == 0)
        {
            OutputStatus = "Select at least one stream destination.";
            OutputSessionStatus = OutputStatus;
            return;
        }

        if (!Streaming && ValidateStreamDestinations() is { Length: > 0 } validationError)
        {
            OutputStatus = validationError;
            OutputSessionStatus = validationError;
            return;
        }

        Streaming = !Streaming;
        RefreshOutputStatus();

        if (_bridge.Running)
        {
            try
            {
                await SyncActiveSceneAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                OutputStatus = ex.Message;
            }
        }
    }

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
    private void SetRoutingMatrixMode(string mode)
    {
        RoutingMatrixMode = mode == "video" ? "video" : "audio";
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
        var sources = BuildAssignedInputSources();
        sources.Add(new RoutingSource("zoom-mix", "Zoom program mix"));
        sources.Add(new RoutingSource("media", "Media playback"));
        AudioRoutingMatrix.Build(sources);
        RefreshAudioProcessingTargets();
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
        if (!cell.SourceId.StartsWith("input-", StringComparison.OrdinalIgnoreCase))
        {
            return;
        }

        RefreshAudioProcessingTargets();
        _ = TrySyncMediaCoreAsync();
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
        if (_audioMixerWindow is not null)
        {
            _audioMixerWindow.Activate();
            return;
        }

        _audioMixerWindow = new AudioMixerWindow(this);
        _audioMixerWindow.WindowClosed += OnAudioMixerWindowClosed;
        _audioMixerWindow.Activate();
    }

    private void OnAudioMixerWindowClosed(object? sender, EventArgs e)
    {
        if (_audioMixerWindow is not null)
        {
            _audioMixerWindow.WindowClosed -= OnAudioMixerWindowClosed;
            _audioMixerWindow = null;
        }
    }

    [RelayCommand]
    private void OpenProductionSettings()
    {
        if (_productionSettingsWindow is not null)
        {
            _productionSettingsWindow.Activate();
            return;
        }

        _productionSettingsWindow = new ProductionSettingsWindow(this);
        _productionSettingsWindow.WindowClosed += OnProductionSettingsWindowClosed;
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

    public void SetMixerManualGain(string participantId, double gainDb)
    {
        var mix = _audioMixChannels.FirstOrDefault(item =>
            string.Equals(item.ParticipantId, participantId, StringComparison.Ordinal));
        if (mix is null)
        {
            return;
        }

        mix.ManualGainDb = Math.Clamp(Math.Round(gainDb, 1), -24, 24);
        RefreshMixerBindings(participantId);
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

        mix.Pan = Math.Clamp(Math.Round(pan, 2), -1, 1);
        RefreshMixerBindings(participantId);
        _ = TrySyncMediaCoreAsync();
    }

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
        RefreshMixerBindings(participantId);
        CommandStatus = mix.Muted
            ? $"{SelectedParticipant?.Name} muted in mix"
            : $"{SelectedParticipant?.Name} unmuted in mix";
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
            ? $"Added {normalized} scan slot. Live VST processing requires the native bridge."
            : $"Added {normalized} to {SelectedParticipant?.Name ?? "selected channel"}";
        RefreshMixerBindings(mix.ParticipantId);
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
        RefreshMixerBindings(mix.ParticipantId);
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
        RefreshAudioParticipantRows();
        RefreshAudioReadoutBindings();
        _ = TrySyncMediaCoreAsync();
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

    private void RefreshMixerBindings(string participantId)
    {
        if (!string.Equals(SelectedParticipantId, participantId, StringComparison.Ordinal))
        {
            SelectedParticipantId = participantId;
        }
        SelectedAudioProcessingTargetId = FormatChannelProcessingTargetId(participantId);

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

        SyncPreviewCanvasLayers(GetMutableRoutes(PreviewSceneId));
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
            CommandStatus = ex.Message;
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
                CommandStatus = ex.Message;
            }
        }
    }

    public IReadOnlyList<GraphicOverlay> EnabledGraphics =>
        Graphics.Where(graphic => graphic.Enabled).ToList();

    public void AddGraphicOverlay(string kind)
    {
        var normalizedKind = string.IsNullOrWhiteSpace(kind) ? "lower-third" : kind.Trim().ToLowerInvariant();
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
        RefreshMultiviewGridTiles();
        CommandStatus = $"{asset.Name} ready for playback";
        _ = TrySyncMediaCoreAsync();
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
        MediaBinGroups = ApplyMediaSelection(MediaBinGroups);

        MediaPlaybackStatus = SelectedMediaAssetPlaying
            ? $"Playing {asset.Name}"
            : $"{asset.Name} paused";
        CommandStatus = MediaPlaybackStatus;

        OnPropertyChanged(nameof(MediaBinGroups));
        OnPropertyChanged(nameof(HasSelectedMediaAsset));
        OnPropertyChanged(nameof(SelectedMediaAssetSummary));
        OnPropertyChanged(nameof(MediaPlaybackButtonLabel));
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

    private async Task TrySyncMediaCoreAsync()
    {
        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(false);
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            CommandStatus = ex.Message;
        }
    }

    private async Task StartMediaCoreOnLaunchAsync()
    {
        try
        {
            await EnsureMediaCoreRunningAsync("Starting media core...").ConfigureAwait(false);
            await SyncActiveSceneAsync().ConfigureAwait(false);
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
            .OrderBy(device => device.SourceKind.Equals("embedded-capture-audio", StringComparison.OrdinalIgnoreCase) ? 1 : 0)
            .ThenBy(device => device.DriverName, StringComparer.OrdinalIgnoreCase)
            .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

        AudioCaptureDevices.Clear();
        foreach (var device in catalog)
        {
            AudioCaptureDevices.Add(device);
        }

        NormalizeCaptureAudioAssignments();
        if (string.IsNullOrWhiteSpace(selectedAudioId) ||
            AudioCaptureDevices.All(device => !string.Equals(device.Id, selectedAudioId, StringComparison.Ordinal)))
        {
            SelectedLocalAudioCaptureDeviceId = AudioCaptureDevices.FirstOrDefault()?.Id ?? string.Empty;
        }
        else if (!string.Equals(SelectedLocalAudioCaptureDeviceId, selectedAudioId, StringComparison.Ordinal))
        {
            SelectedLocalAudioCaptureDeviceId = selectedAudioId;
        }

        RefreshShowInputEditors();
        OnPropertyChanged(nameof(AudioCaptureDevices));
        OnPropertyChanged(nameof(AudioCaptureSourceOptions));
        RefreshLocalAudioSourceBindings();
    }

    private void NormalizeCaptureAudioAssignments()
    {
        var audioById = AudioCaptureDevices.ToDictionary(device => device.Id, StringComparer.Ordinal);
        foreach (var captureDevice in CaptureDevices)
        {
            if (string.IsNullOrWhiteSpace(captureDevice.AssignedAudioDeviceId) ||
                !audioById.TryGetValue(captureDevice.AssignedAudioDeviceId, out var audioDevice))
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
            SelectedAudioMonitorDeviceId = AudioRenderDevices.FirstOrDefault()?.Id ?? string.Empty;
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
            CommandStatus = "SRT ingest source routed. Waiting for libsrt receiver frames.";
            return;
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
        RefreshMixerBindings(mix.ParticipantId);
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

        RunOnUiThread(() => ApplyDiscoveredCaptureDevices(discovered));
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

            CaptureDevices.Add(device);
        }

        NormalizeCaptureAudioAssignments();
        RefreshDualCaptureSourceOptions();
        RefreshCaptureFleetSummary();
        RefreshShowInputEditors();
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
        OnPropertyChanged(nameof(HasCaptureDevices));
        RebuildAudioCaptureDeviceCatalog();
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
            RoomVideoParticipants,
            Scenes,
            AutomationPreferScreenShare,
            (int)Math.Round(AutomationPanelParticipantThreshold));
        FeedHealthRows = ProductionStateHelper.BuildFeedHealthRows(RoomVideoParticipants);
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
        RefreshAudioMonitorBindings();
    }

    private void RefreshAudioMonitorBindings()
    {
        OnPropertyChanged(nameof(SelectedAudioMonitorDeviceName));
        OnPropertyChanged(nameof(AudioMonitorVolumeLabel));
        OnPropertyChanged(nameof(AudioMonitorStatus));
        OnPropertyChanged(nameof(AudioMonitorEngineStatus));
    }

    private void OnAudioMonitorSettingsChanged()
    {
        RefreshAudioMonitorBindings();
        _ = TrySyncMediaCoreAsync();
    }

    private static string FormatMonitorStatus(NativeMediaCoreAudioMixSession audio) =>
        audio.MonitorStatus switch
        {
            "playing" => $"{audio.MonitorFramesPlayed} playback frames",
            "armed" => "armed, waiting for audio",
            "missing-device" => "needs output device",
            "muted" => "muted",
            "unavailable" => "unavailable",
            _ => audio.MonitorStatus ?? "unknown"
        };

    private void RefreshLocalAudioSourceBindings()
    {
        OnPropertyChanged(nameof(SelectedLocalAudioCaptureDeviceName));
        OnPropertyChanged(nameof(LocalAudioSourceStatus));
    }

    private void RefreshAudioMixChannels()
    {
        var existing = _audioMixChannels.ToDictionary(channel => channel.ParticipantId);
        _audioMixChannels.Clear();
        _audioMixChannels.AddRange(
            ProductionStateHelper.BuildAudioMixChannels(RoomVideoParticipants, existing));
    }

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

        return ZoomMediaSpinePayloadBuilder.Build(
            new ZoomMediaSpinePayloadBuilder.BuildInput
            {
                Participants = participants,
                Recording = Recording,
                SelectedBreakoutRoomId = _currentRoomId,
                EngineRunning = ZoomCaptureSubscribed,
                OAuthSignedIn = Settings.ZoomOAuthSignedIn,
                SdkVersion = _bridge.Profile?.Name ?? "zoom-engine",
                SdkRuntimeReady = !Settings.SdkIsBlocked
            });
    }

    private async Task SyncActiveSceneAsync()
    {
        var scene = Scenes.First(s => s.Id == ActiveSceneId);
        var commands = MediaCoreCommandBuilder.BuildSyncCommands(BuildProductionSyncContext());
        var snapshot = await _bridge.SyncAsync(commands).ConfigureAwait(false);
        ApplyLiveProductionPatch(LiveProductionSync.MapSnapshotToStudioPatch(snapshot, BuildLiveProductionContext()));
        CommandStatus = $"{scene.Name} synced to media core";
    }

    private MediaCoreProductionSyncContext BuildProductionSyncContext()
    {
        var sceneRoutes = GetMutableRoutes(ActiveSceneId)
            .Select(ResolveRouteFromShowInput)
            .Select(route => new MediaCoreSceneRouteWire(
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
                ColorGrade: BuildRouteColorGradeWire(route)))
            .ToList();

        var participants = RoomVideoParticipants
            .Select(participant => new MediaCoreParticipantWire(
                participant.Id,
                participant.Name,
                participant.Role.ToString().ToLowerInvariant(),
                participant.BreakoutRoomId,
                participant.BreakoutRoomName,
                participant.IsActiveSpeaker,
                participant.IsMuted,
                participant.IsScreenSharing,
                participant.AudioLevel,
                MapParticipantHealth(participant.Health)))
            .ToList();

        var audioMixByParticipant = AudioMix.Participants.ToDictionary(mix => mix.ParticipantId);
        var audioChannels = RoomVideoParticipants
            .Select(participant =>
            {
                audioMixByParticipant.TryGetValue(participant.Id, out var mix);
                return new MediaCoreAudioMixChannelWire(
                    participant.Id,
                    mix?.OutputLevel ?? participant.AudioLevel,
                    mix?.Muted ?? participant.IsMuted,
                    mix?.NoiseSuppression ?? false,
                    mix?.ManualGainDb == 0 ? null : mix?.ManualGainDb,
                    mix?.Pan ?? 0,
                    mix?.Solo ?? false,
                    mix?.PluginInserts ?? []);
            })
            .ToList();

        var audioRoutingSends = AudioRoutingMatrix.Rows
            .SelectMany(row => row.Cells)
            .Where(cell => cell.IsRouted)
            .Select(cell => new MediaCoreAudioRoutingSendWire(
                cell.SourceId,
                cell.Bus.Id,
                cell.GainDb,
                ResolveAudioProcessingInserts(FormatBusProcessingTargetId(cell.Bus.Id))))
            .ToList();

        var captureAudioSources = CaptureDevices
            .Where(device => !string.Equals(device.Vendor, "srt", StringComparison.OrdinalIgnoreCase))
            .Select(BuildCaptureAudioSourceWire)
            .ToList();
        if (LocalAudioSourceEnabled &&
            AudioCaptureDevices.FirstOrDefault(device =>
                string.Equals(device.Id, SelectedLocalAudioCaptureDeviceId, StringComparison.Ordinal)) is { } localAudio)
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

        var isoParticipantIds = BuildIsoParticipantTargets();

        if (isoParticipantIds.Count == 0)
        {
            isoParticipantIds = MediaCoreProductionSyncContext.DefaultRecordingTargets.IsoParticipantIds.ToList();
        }

        var selectedMediaAsset = SelectedMediaAssetId is null ? null : FindMediaAsset(SelectedMediaAssetId);

        return new MediaCoreProductionSyncContext
        {
            ActiveSceneId = ActiveSceneId,
            SceneRoutes = sceneRoutes,
            Participants = participants,
            Recording = Recording,
            Streaming = Streaming,
            StreamDestinations = BuildSelectedStreamDestinations(),
            StreamDestinationSettings = BuildStreamDestinationSettings(),
            SrtIngestSources = BuildSrtIngestSourceSettings(),
            CanvasOutputProfile = BuildRequestedOutputProfile("canvas", CanvasResolution, CanvasFps, "h264"),
            StreamOutputProfile = BuildRequestedOutputProfile("stream", StreamRenderResolution, StreamRenderFps, StreamVideoCodec),
            RecordingOutputProfile = BuildRequestedOutputProfile("recording", RecordingRenderResolution, RecordingRenderFps, RecordingVideoCodec),
            RecordingTargets = BuildRecordingTargets(isoParticipantIds),
            Graphics = Graphics
                .Select(graphic => new MediaCoreGraphicWire(
                    graphic.Id,
                    graphic.Name,
                    graphic.Position,
                    graphic.Enabled))
                .ToList(),
            LowerThirdKey = ProgramLowerThirdKey.IsVisible
                ? new MediaCoreLowerThirdKeyWire(
                    ProgramLowerThirdKey.SourceId,
                    ProgramLowerThirdKey.SourceName,
                    ProgramLowerThirdKey.Title,
                    ProgramLowerThirdKey.Org,
                    ProgramLowerThirdKey.Position,
                    ProgramLowerThirdKey.Phase,
                    ProgramLowerThirdKey.Enabled)
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
            AudioMonitor = new MediaCoreAudioMonitorWire(
                AudioMonitoringEnabled,
                SelectedAudioMonitorDeviceId,
                SelectedAudioMonitorDeviceName,
                AudioMonitorVolume),
            AudioMixChannels = audioChannels,
            AudioRoutingSends = audioRoutingSends,
            CaptureAudioSources = captureAudioSources,
            CaptionText = CaptionText,
            CaptionSpeaker = CaptionSpeaker,
            SelectedMediaAssetId = SelectedMediaAssetId,
            SelectedMediaAssetName = SelectedMediaAssetName,
            SelectedMediaAssetKind = selectedMediaAsset?.Kind,
            SelectedMediaAssetPath = selectedMediaAsset?.FilePath,
            SelectedMediaAssetPlaying = SelectedMediaAssetPlaying
        };
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

    private IReadOnlyList<string> BuildSelectedStreamDestinations()
    {
        var destinations = new List<string>(3);
        if (StreamRtmpEnabled)
        {
            destinations.Add("rtmp");
        }

        if (StreamNdiEnabled)
        {
            destinations.Add("ndi");
        }

        if (StreamSrtEnabled)
        {
            destinations.Add("srt");
        }

        return destinations;
    }

    private IReadOnlyList<MediaCoreStreamDestinationWire> BuildStreamDestinationSettings()
    {
        var destinations = new List<MediaCoreStreamDestinationWire>(3);
        if (StreamRtmpEnabled)
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "rtmp",
                Label: "RTMP",
                Protocol: StudioStreamOutputValidation.NormalizeRtmpProtocol(StreamRtmpProtocol),
                Url: StudioStreamOutputValidation.BuildRtmpUrl(StreamRtmpProtocol, StreamRtmpServerUrl),
                StreamKey: NormalizeOutputText(StreamRtmpStreamKey, string.Empty)));
        }

        if (StreamNdiEnabled)
        {
            destinations.Add(new MediaCoreStreamDestinationWire(
                Id: "ndi",
                Label: "NDI",
                NdiName: NormalizeOutputText(StreamNdiProgramName, "CoreVideo Pro Program"),
                NdiGroup: NormalizeOutputText(StreamNdiGroupName, "public")));
        }

        if (StreamSrtEnabled)
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
                StreamId: NormalizeOptionalOutputText(StreamSrtStreamId)));
        }

        return destinations;
    }

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
        if (StreamRtmpEnabled && ValidateRtmpSettings() is { Length: > 0 } rtmpError)
        {
            return rtmpError;
        }

        if (StreamNdiEnabled && !IsNdiConfigured())
        {
            return "Configure an NDI program name before streaming.";
        }

        if (StreamSrtEnabled && ValidateSrtSettings() is { Length: > 0 } srtError)
        {
            return srtError;
        }

        return null;
    }

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
            TargetFolder: NormalizeOutputText(RecordingTargetFolder, MediaCoreProductionSyncContext.DefaultRecordingTargets.TargetFolder),
            FilenamePrefix: NormalizeOutputText(RecordingFilenamePrefix, MediaCoreProductionSyncContext.DefaultRecordingTargets.FilenamePrefix),
            Format: NormalizeOutputText(RecordingFormat, MediaCoreProductionSyncContext.DefaultRecordingTargets.Format).ToLowerInvariant(),
            Quality: NormalizeOutputText(RecordingQuality, MediaCoreProductionSyncContext.DefaultRecordingTargets.Quality).ToLowerInvariant(),
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
        string? codec)
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
            TargetBitrateMbps: EstimateTargetBitrateMbps(width, height, fps),
            Codec: NormalizeVideoCodec(codec));
    }

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

    private void OnOutputProfileChanged()
    {
        var canvasProfile = BuildRequestedOutputProfile("canvas", CanvasResolution, CanvasFps, "h264");
        ProgramResolutionLabel = TransportFormatting.ShortResolutionLabel(canvasProfile.Resolution, canvasProfile.Fps);
        OnPropertyChanged(nameof(CanvasProfileSummary));
        OnPropertyChanged(nameof(StreamRenderProfileSummary));
        OnPropertyChanged(nameof(RecordingRenderProfileSummary));
        OnPropertyChanged(nameof(StreamConfigurationSummary));
        OnPropertyChanged(nameof(RecordingOptionsSummary));
        RefreshTransportState();

        if (_bridge.Running)
        {
            _ = TrySyncMediaCoreAsync();
        }
    }

    private async void OnStreamOutputOptionChanged()
    {
        OnPropertyChanged(nameof(StreamDestinationSummary));
        OnPropertyChanged(nameof(StreamConfigurationSummary));
        OnPropertyChanged(nameof(StreamRtmpSummary));
        OnPropertyChanged(nameof(StreamNdiSummary));
        OnPropertyChanged(nameof(StreamSrtSummary));

        if (!Streaming || !_bridge.Running)
        {
            return;
        }

        try
        {
            await SyncActiveSceneAsync().ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            OutputStatus = ex.Message;
        }
    }

    private async void OnRecordingOutputOptionChanged()
    {
        OnPropertyChanged(nameof(RecordingOptionsSummary));

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
            OutputStatus = ex.Message;
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
    }

    private void OnSnapshotChanged(NativeMediaCoreStateSnapshot snapshot) =>
        RunOnUiThread(() => ApplySnapshotChanged(snapshot));

    private void ApplySnapshotChanged(NativeMediaCoreStateSnapshot snapshot)
    {
        // The compositor is always on, so surfaces always accept the latest program frame.
        _surfaces.OnMediaCoreSnapshot(snapshot);

        // Always apply meeting/ZoomStatus/roster fields from the snapshot BEFORE any
        // early-return so meeting status keeps updating even when capture is unsubscribed.
        ApplyMeetingFieldsFromSnapshot(snapshot);
        var programResolutionLabel = ResolveProgramResolutionLabel(snapshot);
        ProgramResolutionLabel = programResolutionLabel;
        Transport.ApplySnapshot(
            snapshot,
            snapshot.Recording?.Active == true,
            LiveProductionSync.IsStreamingLive(snapshot),
            programResolutionLabel,
            MasterLimiterEnabled);
        RefreshAudioReadoutBindings();

        if (!ZoomCaptureSubscribed)
        {
            return;
        }

        var liveProductionContext = BuildLiveProductionContext();
        var autoStopStatus = LiveProductionSync.ResolveBreakoutRoomAutoStopStatus(
            snapshot,
            liveProductionContext.CurrentBreakoutRoomId);
        if (autoStopStatus is not null)
        {
            StopEngineForBreakoutRoomChange(autoStopStatus);
            return;
        }

        var patch = LiveProductionSync.MapSnapshotToStudioPatch(snapshot, liveProductionContext);
        ApplyLiveProductionPatch(patch);
        ApplyGraphicsAndCaptionStateFromSnapshot(snapshot);
    }

    private LiveProductionSync.LiveProductionSyncContext BuildLiveProductionContext() =>
        new()
        {
            ActiveSceneId = ActiveSceneId,
            ActiveSceneLayout = ProgramScene.Layout,
            CurrentBreakoutRoomId = _currentRoomId,
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

    private static int LiveParticipantCountFromPatch(LiveProductionSync.StudioLiveProductionPatch patch) =>
        patch.Participants?.Count ?? 0;

    private void ApplyLiveParticipants(IReadOnlyList<LiveProductionSync.LiveProductionParticipantContext> participants)
    {
        var mapped = ParticipantMapper.ToParticipants(participants);
        RoomVideoParticipants = ParticipantMapper.VideoParticipantsInRoom(mapped, _currentRoomId);
        CurrentRoomLabel = _currentRoomName;
        MultiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
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
            ZoomStatus = "Zoom Live";
            CurrentRoomLabel = _currentRoomName;
            ApplyCaptionAndLowerThirdPatch(patch);
            ApplyCaptionTranscriptFromSnapshot(snapshot);
            var participants = LiveProductionSync.MapSnapshotParticipants(snapshot);
            Settings.ApplyMeetingStateLabel(meetingState, participants?.Count ?? snapshot.Participants.Count);
            if (participants is { Count: > 0 })
            {
                ApplyLiveParticipants(participants);
                SyncShowInputsFromMeeting(participants);
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
        var validIds = participants.Select(participant => participant.Id).ToHashSet(StringComparer.Ordinal);
        foreach (var slot in ShowInputs)
        {
            if (slot.Kind == ShowInputKind.ZoomParticipant &&
                !string.IsNullOrWhiteSpace(slot.ParticipantId) &&
                !validIds.Contains(slot.ParticipantId))
            {
                slot.Kind = ShowInputKind.Unassigned;
                slot.ParticipantId = null;
                slot.InShow = false;
            }
        }

        if (!ShowInputs.Any(slot => slot.InShow && slot.IsAssigned))
        {
            foreach (var (participant, index) in participants.Take(ShowInputRosterService.MaxMultiviewBoxes).Select((item, i) => (item, i)))
            {
                if (index >= ShowInputs.Count)
                {
                    break;
                }

                var slot = ShowInputs[index];
                slot.Kind = ShowInputKind.ZoomParticipant;
                slot.ParticipantId = participant.Id;
                slot.InShow = true;
            }
        }

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
        AudioParticipantRows = RoomVideoParticipants
            .Select(participant =>
            {
                var mix = _audioMixChannels.FirstOrDefault(m => m.ParticipantId == participant.Id);
                return new AudioParticipantRow
                {
                    Id = participant.Id,
                    Name = participant.Name,
                    Subtitle = $"{participant.RoleLabel} · {participant.BreakoutRoomName} · {participant.HealthLabel}",
                    OutputLevel = mix?.OutputLevel ?? participant.AudioLevel,
                    ManualGainDb = mix?.ManualGainDb ?? 0,
                    Pan = mix?.Pan ?? 0,
                    Lufs = mix?.Lufs ?? -60,
                    TruePeakDb = mix?.TruePeakDb ?? -60,
                    Muted = mix?.Muted ?? participant.IsMuted,
                    GainLabel = mix is null ? "0.0 dB" : $"{(mix.ManualGainDb > 0 ? "+" : "")}{mix.ManualGainDb:0.0} dB",
                    PanLabel = mix is null || Math.Abs(mix.Pan) < 0.01
                        ? "C"
                        : mix.Pan < 0
                            ? $"L {Math.Abs(mix.Pan):0.00}"
                            : $"R {mix.Pan:0.00}",
                    LufsLabel = mix is null ? "-60.0 LUFS" : $"{mix.Lufs:0.0} LUFS",
                    TruePeakLabel = mix is null ? "-60.0 dBTP" : $"{mix.TruePeakDb:0.0} dBTP",
                    BusLabel = ResolvePrimaryAudioBusLabel(participant.Id),
                    InsertLabel = mix is null || mix.PluginInserts.Count == 0
                        ? "No inserts"
                        : string.Join(" + ", mix.PluginInserts),
                    MuteButtonLabel = mix?.Muted == true ? "Unmute" : "Mute",
                    MuteStateLabel = mix?.Muted == true ? "Muted" : "Live",
                    IsSelected = participant.Id == SelectedParticipantId
                };
            })
            .ToList();
        OnPropertyChanged(nameof(AudioParticipantRows));
        RefreshAudioProcessingTargets();
    }

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

        foreach (var bus in AudioRoutingMatrix.BusHeaders)
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

        AudioProcessingTargetOptions = targets;
        if (AudioProcessingTargetOptions.Count > 0 &&
            AudioProcessingTargetOptions.All(target =>
                !string.Equals(target.Id, SelectedAudioProcessingTargetId, StringComparison.Ordinal)))
        {
            SelectedAudioProcessingTargetId = AudioProcessingTargetOptions.First().Id;
        }

        OnPropertyChanged(nameof(AudioProcessingTargetOptions));
        OnPropertyChanged(nameof(SelectedAudioProcessingTarget));
        OnPropertyChanged(nameof(SelectedAudioProcessingTargetLabel));
        OnPropertyChanged(nameof(SelectedAudioProcessingTargetKindLabel));
        OnPropertyChanged(nameof(SelectedAudioProcessingTargetDetail));
        OnPropertyChanged(nameof(SelectedAudioProcessingInsertLabel));
        OnPropertyChanged(nameof(ProcessingBridgeStatusLabel));
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
            .Where(row => TryResolveRoutingSourceParticipantId(row.SourceId, out var routedParticipantId) &&
                          string.Equals(routedParticipantId, participantId, StringComparison.Ordinal))
            .SelectMany(row => row.Cells)
            .Where(cell => cell.IsRouted)
            .Select(cell => cell.Bus.Label)
            .FirstOrDefault();

        return routedBus ?? "MASTER";
    }

    private void OnSurfacesChanged() => RunOnUiThread(RefreshSurfaceBindings);

    private void RefreshSurfaceBindings()
    {
        ProgramSurface = _surfaces.ProgramSurface;
        PreviewSurface = _surfaces.PreviewSurface;
        MultiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
        RefreshOpenColorGradeEditorPreviews();
        SchedulePreviewRoutingRefresh();
        ScheduleMultiviewGridRefresh();
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

    private void InitializeShowInputEditors()
    {
        ShowInputEditors.Clear();
        foreach (var slot in ShowInputs)
        {
            ShowInputEditors.Add(new ShowInputSlotViewModel(slot, OnShowInputChanged, SetCaptureDeviceAudioSource));
        }

        RefreshShowInputEditors();
    }

    private void RefreshShowInputEditors()
    {
        foreach (var editor in ShowInputEditors)
        {
            editor.RefreshSourceOptions(RoomVideoParticipants, CaptureDevices, AudioCaptureDevices);
        }

        OnPropertyChanged(nameof(ShowInputSummary));
        OnPropertyChanged(nameof(MultiviewHeader));
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
        CommandStatus = "Show input roster updated";
    }

    private void QueueSelectedCaptureDevicesOnline()
    {
        foreach (var deviceId in ShowInputs
                     .Where(slot => slot.InShow &&
                         slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest &&
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
        try
        {
            await ConnectCaptureDeviceAsync(deviceId).ConfigureAwait(false);
        }
        finally
        {
            RunOnUiThread(() => _captureAutoConnectInFlight.Remove(deviceId));
        }
    }

    private MediaAsset? FindMediaAsset(string assetId) =>
        MediaBinGroups
            .SelectMany(group => group.Assets)
            .FirstOrDefault(candidate => string.Equals(candidate.Id, assetId, StringComparison.Ordinal));

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
                RelativePath = asset.RelativePath,
                FilePath = asset.FilePath,
                FileType = asset.FileType,
                IsSelected = string.Equals(asset.Id, SelectedMediaAssetId, StringComparison.Ordinal)
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

    private void AssignConnectedCaptureDeviceToShowInput(CaptureDevice device)
    {
        var targetKind = ResolveShowInputKind(device);
        var existing = ShowInputs.FirstOrDefault(slot =>
            slot.Kind == targetKind &&
            string.Equals(slot.CaptureDeviceId, device.Id, StringComparison.Ordinal));
        if (existing is not null)
        {
            existing.InShow = true;
            return;
        }

        var slot = ShowInputs.FirstOrDefault(slot => slot.InShow && !slot.IsAssigned) ??
            ShowInputs.FirstOrDefault(slot => !slot.InShow && !slot.IsAssigned);
        if (slot is null)
        {
            return;
        }

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
            _ => ShowInputKind.UvcWebcam
        };

    private static bool IsVirtualSrtIngestDevice(CaptureDevice device) =>
        string.Equals(device.Vendor, "srt", StringComparison.OrdinalIgnoreCase);

    private void RefreshMultiviewGridTiles()
    {
        var tiles = ShowInputRosterService.BuildMultiviewTiles(
            ShowInputs,
            RoomVideoParticipants,
            CaptureDevices,
            MultiviewTiles,
            _surfaces.CaptureDeviceSurfaces).ToList();

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
                    SelectedMediaAssetPlaying),
                SourceIndex = tiles.Count + 1
            });
        }

        MultiviewGridTiles = tiles;
        OnPropertyChanged(nameof(MultiviewHeader));
    }

    private void RefreshOutputStatus()
    {
        // Recording/streaming run against the always-on core, so reflect the live snapshot
        // whenever the core is running — capture does not have to be subscribed.
        if (_bridge.Running && _bridge.LastSnapshot is { } snapshot)
        {
            OutputStatus = MediaCoreBridgeService.SummarizeOutputs(snapshot);
            OutputSessionStatus = LiveProductionSync.SummarizeOutputSession(snapshot);
            return;
        }

        if (!Recording && !Streaming)
        {
            OutputStatus = "Outputs idle";
            OutputSessionStatus = "Outputs idle";
            return;
        }

        var localStatus = LiveProductionSync.SummarizeLocalOutputs(Recording, Streaming);
        OutputStatus = localStatus;
        OutputSessionStatus = localStatus;
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
            return;
        }

        Transport.ApplyIdleState(Recording, Streaming, ProgramResolutionLabel);
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
                SelectCommand = SelectSceneCommand
            })
            .ToList();
        OnPropertyChanged(nameof(SceneItems));
        OnPropertyChanged(nameof(SceneRailDisplaySummary));
    }

    private static bool IsInternalMultiviewSoloScene(string sceneId) =>
        string.Equals(sceneId, MultiviewSoloSceneAId, StringComparison.Ordinal) ||
        string.Equals(sceneId, MultiviewSoloSceneBId, StringComparison.Ordinal);

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
        RefreshSceneCompositionState(PreviewScene, PreviewSceneId, isPreview: true);
        RefreshSceneCompositionState(ProgramScene, ActiveSceneId, isPreview: false);
    }

    public void SetCanvasInteractionActive(bool isActive) =>
        _canvasInteractionActive = isActive;

    private void RefreshSceneCompositionState(Scene scene, string sceneId, bool isPreview)
    {
        var mutableRoutes = GetMutableRoutes(sceneId);
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
            var sceneTiles = BuildSceneTiles(scene, workingRoutes);
            ProgramSceneRoutes = workingRoutes;
            ProgramSceneTiles = sceneTiles;
            OnPropertyChanged(nameof(ProgramSceneRoutes));
            OnPropertyChanged(nameof(ProgramSceneTiles));
            UpdateProgramLowerThirdKey(ResolveProgramLowerThirdSource(workingRoutes));
        }
    }

    public void RefreshProgramLowerThirdKeyPosition() =>
        UpdateProgramLowerThirdKey(ResolveProgramLowerThirdSource(ProgramSceneRoutes), force: true);

    private void UpdateProgramLowerThirdKey(LowerThirdSource? source, bool force = false)
    {
        var lowerThirdEnabled = AutomationLowerThirdsEnabled ||
            Graphics.Any(graphic => graphic.Enabled && graphic.Kind.Equals("lower-third", StringComparison.OrdinalIgnoreCase));

        if (!lowerThirdEnabled || source is null)
        {
            _lowerThirdKeyTransitionCts?.Cancel();
            ProgramLowerThirdKey = LowerThirdKeyState.Hidden(Overlays.LowerThirdPosition);
            _ = TrySyncMediaCoreAsync();
            return;
        }

        if (!force &&
            ProgramLowerThirdKey.Enabled &&
            string.Equals(ProgramLowerThirdKey.SourceId, source.SourceId, StringComparison.Ordinal) &&
            string.Equals(ProgramLowerThirdKey.SourceName, source.SourceName, StringComparison.Ordinal))
        {
            var next = BuildLowerThirdKey(source, "on-air");
            if (ProgramLowerThirdKey != next)
            {
                ProgramLowerThirdKey = next;
                _ = TrySyncMediaCoreAsync();
            }
            return;
        }

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
                _ = TrySyncMediaCoreAsync();
                await Task.Delay(160, cancellationToken).ConfigureAwait(true);
            }

            ProgramLowerThirdKey = BuildLowerThirdKey(source, "building-in");
            LowerThirdName = source.SourceName;
            LowerThirdTitle = source.Title;
            LowerThirdOrg = source.Org;
            _ = TrySyncMediaCoreAsync();
            await Task.Delay(220, cancellationToken).ConfigureAwait(true);
            ProgramLowerThirdKey = BuildLowerThirdKey(source, "on-air");
            _ = TrySyncMediaCoreAsync();
        }
        catch (OperationCanceledException)
        {
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
            Enabled = true
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

        return sources.FirstOrDefault(source => source.IsActiveSpeaker)
               ?? sources.FirstOrDefault(source => source.IsScreenShare)
               ?? sources[0];
    }

    private LowerThirdSource? ResolveLowerThirdSource(SourceRoute route)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice && route.CaptureDeviceId is { Length: > 0 } captureDeviceId)
        {
            var device = CaptureDevices.FirstOrDefault(item =>
                string.Equals(item.Id, captureDeviceId, StringComparison.Ordinal));
            return device is null
                ? null
                : new LowerThirdSource(
                    $"capture:{device.Id}",
                    device.Name,
                    device.Vendor,
                    device.ResolutionLabel,
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
            participant.Name,
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
                PreviewCanvasLayers[index].SyncFromRoute(RoomVideoParticipants, CaptureDevices, ShowInputs);
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
        PreviewSceneTiles = BuildSceneTiles(scene, workingRoutes);
        OnPropertyChanged(nameof(PreviewRouteWarnings));
        OnPropertyChanged(nameof(HasPreviewRouteWarnings));
        OnPropertyChanged(nameof(PreviewSceneTiles));
        OnPropertyChanged(nameof(PreviewSceneRoutes));
        OnPropertyChanged(nameof(PreviewSceneParticipants));
        OnPropertyChanged(nameof(PreviewSlotCount));
        OnPropertyChanged(nameof(HasPreviewSlotEditors));
        OnPropertyChanged(nameof(SceneBuilderSlotSummary));
    }

    private IReadOnlyList<ParticipantSurfaceTile> BuildSceneTiles(Scene scene, IReadOnlyList<SourceRoute> routes)
    {
        var tilesByParticipant = MultiviewTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        var sceneTiles = new List<ParticipantSurfaceTile>();
        foreach (var route in routes)
        {
            if (ResolveRouteTile(route, tilesByParticipant) is { } routeTile)
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
        if (ResolveRouteTile(resolved, tilesByParticipant) is { } tile)
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
        IReadOnlyDictionary<string, ParticipantSurfaceTile> tilesByParticipant)
    {
        if (route.Mode == SourceRouteMode.CaptureDevice)
        {
            return BuildCaptureSceneTile(route.CaptureDeviceId);
        }

        var participant = SceneRoutingService.ResolveRouteParticipant(route, RoomVideoParticipants);
        return participant is null ? null : BuildParticipantSceneTile(participant, tilesByParticipant);
    }

    private SourceRoute ResolveRouteFromShowInput(SourceRoute route)
    {
        var resolved = route.Clone();
        if (route.ShowInputSlotNumber is not { } slotNumber ||
            ShowInputs.FirstOrDefault(slot => slot.SlotNumber == slotNumber) is not { } slot ||
            !slot.IsAssigned)
        {
            return resolved;
        }

        if (slot.Kind == ShowInputKind.ZoomParticipant)
        {
            resolved.Mode = SourceRouteMode.Fixed;
            resolved.ParticipantId = slot.ParticipantId;
            resolved.CaptureDeviceId = null;
            ApplyKnownColorGradeToRoute(resolved);
            return resolved;
        }

        if (slot.Kind is ShowInputKind.Blackmagic or ShowInputKind.Aja or ShowInputKind.UvcWebcam or ShowInputKind.SrtIngest)
        {
            resolved.Mode = SourceRouteMode.CaptureDevice;
            resolved.ParticipantId = null;
            resolved.CaptureDeviceId = slot.CaptureDeviceId;
        }

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

    public void ApplyCanvasPreset(string presetWire)
    {
        var routes = GetMutableRoutes(PreviewSceneId);
        SceneCanvasLayoutService.ApplyPreset(presetWire, routes);
        CommandStatus = $"{PreviewScene.Name} canvas preset applied ({presetWire})";
        SyncPreviewCanvasLayers(routes);
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        SchedulePreviewRoutingRefresh();
    }

    public void CommitPreviewCanvasLayer(SceneCanvasLayerViewModel layer) =>
        OnPreviewCanvasLayerChanged(layer);

    private void OnPreviewCanvasLayerChanged(SceneCanvasLayerViewModel layer)
    {
        var routes = GetMutableRoutes(PreviewSceneId);
        if (layer.LayerIndex < 0 || layer.LayerIndex >= routes.Count)
        {
            return;
        }

        layer.ApplyRoute();
        SceneRoutingService.ApplyNormalizeRouteUpdate(routes[layer.LayerIndex], RoomVideoParticipants);
        ApplyKnownColorGradeToRoute(routes[layer.LayerIndex]);
        layer.SetSurface(ResolveLayerSurface(routes[layer.LayerIndex], layer.LayerIndex));

        CommandStatus = $"{PreviewScene.Name} source {layer.LayerIndex + 1} updated on canvas";
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(ResolveRouteFromShowInput).ToList());
        OnPropertyChanged(nameof(PreviewCanvasLayers));
    }

    private void CopyPreviewRoutesToScene(string sceneId)
    {
        var previewRoutes = GetMutableRoutes(PreviewSceneId)
            .Select(route => route.Clone())
            .ToList();
        GetMutableRoutes(sceneId).Clear();
        GetMutableRoutes(sceneId).AddRange(previewRoutes);
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
        _bridge.SnapshotChanged -= OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived -= OnZoomVideoFrameReceived;
        _bridge.ProgramFramePreviewReceived -= OnProgramFramePreviewReceived;
        _bridge.ProgramSharedTextureReceived -= _surfaces.OnProgramSharedTexture;
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

    private sealed record LowerThirdSource(
        string SourceId,
        string SourceName,
        string Title,
        string Org,
        bool IsActiveSpeaker,
        bool IsScreenShare);
}
