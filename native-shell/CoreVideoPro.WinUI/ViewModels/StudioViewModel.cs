using System.Collections.ObjectModel;
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

    private readonly MediaCoreBridgeService _bridge = new();
    private readonly MediaBinService _mediaBinService = new();
    private readonly CaptureDeviceDiscoveryService _captureDiscovery = new();
    private readonly VideoSurfaceCoordinator _surfaces = new();
    private readonly DispatcherQueue _dispatcher = DispatcherQueue.GetForCurrentThread();
    private readonly ZoomOAuthService _zoomOAuth;
    private readonly ZoomOAuthAppCoordinator _zoomOAuthCoordinator;
    private readonly string _currentRoomId;
    private readonly string _currentRoomName;

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
    private bool _streamRtmpEnabled = true;

    [ObservableProperty]
    private bool _streamNdiEnabled;

    [ObservableProperty]
    private bool _streamSrtEnabled;

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

    [ObservableProperty]
    private ProductionMode _productionMode = ProductionMode.Manual;

    [ObservableProperty]
    private string _magicSceneStatus = "Join a meeting to enable Magic Scene";

    [ObservableProperty]
    private string _autoProductionReadout = "Join a Zoom meeting to enable scene recommendations.";

    [ObservableProperty]
    private string _automationButtonLabel = "Automation disabled";

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
    private string _feedHealthSummary = "No Zoom feeds — join a meeting";

    [ObservableProperty]
    private string _previewSceneParticipants = "No sources assigned";

    private readonly Dictionary<string, List<SourceRoute>> _sceneRoutes = new(StringComparer.Ordinal);
    // Per-source color grades keyed by participant/source id. UI-only this round: native
    // compositing still receives the single global grade (see OpenColorGradeEditor).
    private readonly Dictionary<string, ColorGrade> _sourceColorGrades = new(StringComparer.Ordinal);
    private bool _previewRoutingRefreshScheduled;
    private bool _showInputRefreshScheduled;
    private bool _multiviewGridRefreshScheduled;
    private bool _canvasInteractionActive;

    public SettingsViewModel Settings { get; }

    public TransportViewModel Transport { get; }

    public OverlaysViewModel Overlays { get; }

    public ObservableCollection<GraphicOverlay> Graphics { get; } = [];

    public ObservableCollection<CaptureDevice> CaptureDevices { get; } = [];

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

    public string PreviewSceneSummary => PreviewScene.Name;

    public string LoudnessTargetLabel =>
        $"target {(int)Math.Round(_bridge.LastSnapshot?.AudioMixSession.LoudnessLufs ?? -16)} LUFS";

    public string LoudnessLevelLabel =>
        _bridge.LastSnapshot?.AudioMixSession.LimiterActive == true ? "limiting" : "on target";

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

    public int CamerasOnCount => RoomVideoParticipants.Count;

    public string ScreenShareLabel => RoomVideoParticipants.Any(p => p.IsScreenSharing) ? "Active" : "Off";

    public string AutoProductionReason => _automationRecommendation.Reason;

    public AudioMixState AudioMix => new()
    {
        Participants = _audioMixChannels,
        LoudnessLufs = _bridge.LastSnapshot?.AudioMixSession.LoudnessLufs ?? -16,
        LimiterActive = _bridge.LastSnapshot?.AudioMixSession.LimiterActive ?? false,
        Summary = ProductionStateHelper.BuildAudioMixSummary(RoomVideoParticipants)
    };

    public StudioViewModel()
    {
        ExternalUriLauncher.BindDispatcher(Microsoft.UI.Dispatching.DispatcherQueue.GetForCurrentThread());

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
        _surfaces.SurfacesChanged += RefreshSurfaceBindings;

        MediaBinGuidance = MediaBinClassifier.BuildEmptyGuidanceMessage();
        _captureDiscovery.StartWatching(() => _ = RefreshCaptureDevicesAsync());
        _ = RefreshCaptureDevicesAsync();
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

    public string ProgramResolutionLabel =>
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
        "Open the Scenes tab to drag sources on the 16:9 canvas like OBS";

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

    partial void OnStreamRtmpEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamNdiEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnStreamSrtEnabledChanged(bool value) => OnStreamOutputOptionChanged();

    partial void OnRecordingTargetFolderChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnRecordingFilenamePrefixChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnRecordingFormatChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnRecordingQualityChanged(string value) => OnRecordingOutputOptionChanged();

    partial void OnProductionModeChanged(ProductionMode value)
    {
        RefreshTransportAutomationState();
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
        OnPropertyChanged(nameof(CanTake));
        TakeCommand.NotifyCanExecuteChanged();
    }

    partial void OnPreviewSceneIdChanged(string value)
    {
        RefreshSceneItems();
        OnPropertyChanged(nameof(PreviewScene));
        OnPropertyChanged(nameof(PreviewSceneSummary));
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
        RefreshAudioParticipantRows();
    }

    partial void OnProgramSurfaceChanged(VideoSurfaceState value)
    {
        OnPropertyChanged(nameof(ProgramResolutionLabel));
        RefreshTransportState();
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

    public bool CanTake => PreviewSceneId != ActiveSceneId;

    private static string NewCustomSceneId() =>
        $"custom-{Guid.NewGuid():N}".Substring(0, "custom-".Length + 8);

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
        ActiveSceneId = PreviewSceneId;
        CopyPreviewRoutesToScene(ActiveSceneId);
        RefreshPreviewRoutingState();
        var scene = Scenes.First(s => s.Id == ActiveSceneId);
        CommandStatus = $"{scene.Name} taken with fade";
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
        foreach (var input in ShowInputEditors)
        {
            if (input.Kind != ShowInputKind.Unassigned && !string.IsNullOrWhiteSpace(input.SelectedSourceId))
            {
                sources.Add(new RoutingSource($"input-{input.SlotNumber:00}", input.SlotLabel));
            }
        }

        return sources;
    }

    private void BuildAudioRoutingMatrix()
    {
        var sources = BuildAssignedInputSources();
        sources.Add(new RoutingSource("zoom-mix", "Zoom program mix"));
        sources.Add(new RoutingSource("media", "Media playback"));
        AudioRoutingMatrix.Build(sources);
    }

    private void BuildVideoRoutingMatrix()
    {
        var sources = BuildAssignedInputSources();
        sources.Add(new RoutingSource("active-speaker", "Active Speaker"));
        sources.Add(new RoutingSource("screen-share", "Screen Share"));
        sources.Add(new RoutingSource("media", "Media"));
        VideoRoutingMatrix.Build(sources);
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
    }

    /// <summary>
    /// Opens the per-source color grade pop-out for the given participant/source id. Seeds
    /// the editor with the source's stored grade (or the current global grade as a default).
    /// On Save the grade is stored per source and the engine is re-synced.
    ///
    /// NOTE (UI-only / Option A): the media-core protocol still carries a single global
    /// grade, so the value sent to native remains the primary/global grade for now. Wiring
    /// per-source grades through MediaCoreCommandBuilder is a follow-up protocol change.
    /// </summary>
    [RelayCommand]
    private void OpenColorGradeEditor(string? sourceId)
    {
        if (string.IsNullOrWhiteSpace(sourceId))
        {
            CommandStatus = "Select a source before editing its color grade";
            return;
        }

        var participant = RoomVideoParticipants.FirstOrDefault(p => p.Id == sourceId);
        var sourceName = participant?.Name ?? sourceId;

        var seed = _sourceColorGrades.TryGetValue(sourceId, out var stored) ? stored : ColorGrade;
        var editorViewModel = new ColorGradeEditorViewModel(sourceId, sourceName, seed);
        editorViewModel.GradeSaved += OnSourceColorGradeSaved;

        var window = new ColorGradeEditorWindow(editorViewModel);
        window.Activate();
    }

    private void OnSourceColorGradeSaved(object? sender, ColorGrade grade)
    {
        if (sender is not ColorGradeEditorViewModel editorViewModel)
        {
            return;
        }

        editorViewModel.GradeSaved -= OnSourceColorGradeSaved;
        _sourceColorGrades[editorViewModel.SourceId] = grade;
        CommandStatus = $"Color grade saved for {editorViewModel.SourceName}: {grade.Summary}";

        // Trigger the normal sync. The global grade is still what reaches native this round;
        // per-source native compositing is a follow-up protocol change (see OpenColorGradeEditor).
        _ = SyncColorGradeChangeAsync();
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
        SchedulePreviewRoutingRefresh();
        CommandStatus = $"{sceneName} queued by Magic Scene";
        RefreshSceneItems();
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

    [RelayCommand]
    private void ConnectCaptureDevice(string deviceId)
    {
        var device = CaptureDevices.FirstOrDefault(d => d.Id == deviceId);
        if (device is null || device.ConnectionState == CaptureConnectionState.Connected)
        {
            return;
        }

        device.ConnectionState = CaptureConnectionState.Connected;
        device.SignalPresent = true;
        AssignConnectedCaptureDeviceToShowInput(device);
        RefreshCaptureFleetSummary();
        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
        CommandStatus = $"{device.Name} brought online as program source";
    }

    [RelayCommand]
    private void ToggleSelectedParticipantMute()
    {
        var mix = SelectedAudioMix;
        if (mix is null)
        {
            return;
        }

        mix.Muted = !mix.Muted;
        OnPropertyChanged(nameof(SelectedAudioMix));
        OnPropertyChanged(nameof(SelectedMuteButtonLabel));
        CommandStatus = mix.Muted
            ? $"{SelectedParticipant?.Name} muted in mix"
            : $"{SelectedParticipant?.Name} unmuted in mix";
    }

    private void RefreshCaptureFleetSummary()
    {
        CaptureFleetSummary = ProductionStateHelper.CaptureFleetSummary(CaptureDevices);
        DualCaptureLive = ProductionStateHelper.DualCaptureLive(CaptureDevices);
    }

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
        foreach (var device in discovered)
        {
            if (priorById.TryGetValue(device.Id, out var prior))
            {
                device.ConnectionState = prior.ConnectionState;
                device.SignalPresent = prior.SignalPresent;
                device.SelectedInputId = prior.SelectedInputId;
                device.AudioSyncOffsetMs = prior.AudioSyncOffsetMs;
            }

            CaptureDevices.Add(device);
        }

        RefreshCaptureFleetSummary();
        RefreshShowInputEditors();
        RefreshMultiviewGridTiles();
        OnPropertyChanged(nameof(HasCaptureDevices));
    }

    private void RefreshProductionReadouts()
    {
        _automationRecommendation = ProductionStateHelper.BuildAutomationRecommendation(
            RoomVideoParticipants,
            Scenes);
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
        OnPropertyChanged(nameof(CaptionQualitySummary));
        OnPropertyChanged(nameof(AudioMix));
        OnPropertyChanged(nameof(LoudnessTargetLabel));
        OnPropertyChanged(nameof(LoudnessLevelLabel));
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
            .Select(route => new MediaCoreSceneRouteWire(
                route.Id,
                SceneRoutingService.ModeToWire(route.Mode),
                SceneRoutingService.AudioRoleToWire(route.AudioRole),
                route.ParticipantId,
                route.CanvasRect?.X,
                route.CanvasRect?.Y,
                route.CanvasRect?.Width,
                route.CanvasRect?.Height,
                route.ZIndex))
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
                    mix?.ManualGainDb == 0 ? null : mix?.ManualGainDb);
            })
            .ToList();

        var audioRoutingSends = AudioRoutingMatrix.Rows
            .SelectMany(row => row.Cells)
            .Where(cell => cell.IsRouted)
            .Select(cell => new MediaCoreAudioRoutingSendWire(cell.SourceId, cell.Bus.Id, cell.GainDb))
            .ToList();

        var isoParticipantIds = GetMutableRoutes(ActiveSceneId)
            .Where(route =>
                route.AudioRole == SourceAudioRole.Isolated &&
                route.ParticipantId is not null)
            .Select(route => route.ParticipantId!)
            .Distinct(StringComparer.Ordinal)
            .OrderBy(id => id, StringComparer.Ordinal)
            .ToList();

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
            RecordingTargets = BuildRecordingTargets(isoParticipantIds),
            Graphics = Graphics
                .Select(graphic => new MediaCoreGraphicWire(
                    graphic.Id,
                    graphic.Name,
                    graphic.Position,
                    graphic.Enabled))
                .ToList(),
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
            AudioMixChannels = audioChannels,
            AudioRoutingSends = audioRoutingSends,
            CaptionText = CaptionText,
            CaptionSpeaker = CaptionSpeaker,
            SelectedMediaAssetId = SelectedMediaAssetId,
            SelectedMediaAssetName = SelectedMediaAssetName,
            SelectedMediaAssetKind = selectedMediaAsset?.Kind,
            SelectedMediaAssetPath = selectedMediaAsset?.FilePath,
            SelectedMediaAssetPlaying = SelectedMediaAssetPlaying
        };
    }

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

    private MediaCoreRecordingTargetsWire BuildRecordingTargets(IReadOnlyList<string> isoParticipantIds) =>
        new(
            TargetFolder: NormalizeOutputText(RecordingTargetFolder, MediaCoreProductionSyncContext.DefaultRecordingTargets.TargetFolder),
            FilenamePrefix: NormalizeOutputText(RecordingFilenamePrefix, MediaCoreProductionSyncContext.DefaultRecordingTargets.FilenamePrefix),
            Format: NormalizeOutputText(RecordingFormat, MediaCoreProductionSyncContext.DefaultRecordingTargets.Format).ToLowerInvariant(),
            Quality: NormalizeOutputText(RecordingQuality, MediaCoreProductionSyncContext.DefaultRecordingTargets.Quality).ToLowerInvariant(),
            IsoParticipantIds: isoParticipantIds);

    private static string NormalizeOutputText(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value.Trim();

    private async void OnStreamOutputOptionChanged()
    {
        OnPropertyChanged(nameof(StreamDestinationSummary));

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
        Transport.ApplySnapshot(
            snapshot,
            snapshot.Recording?.Active == true,
            LiveProductionSync.IsStreamingLive(snapshot),
            ResolveProgramResolutionLabel(snapshot));
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
                    IsSelected = participant.Id == SelectedParticipantId
                };
            })
            .ToList();
        OnPropertyChanged(nameof(AudioParticipantRows));
    }

    private void RefreshSurfaceBindings()
    {
        ProgramSurface = _surfaces.ProgramSurface;
        PreviewSurface = _surfaces.PreviewSurface;
        MultiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
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
            ShowInputEditors.Add(new ShowInputSlotViewModel(slot, OnShowInputChanged));
        }

        RefreshShowInputEditors();
    }

    private void RefreshShowInputEditors()
    {
        foreach (var editor in ShowInputEditors)
        {
            editor.RefreshSourceOptions(RoomVideoParticipants, CaptureDevices);
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
        CommandStatus = "Show input roster updated";
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
            "uvc" or "windows" => ShowInputKind.UvcWebcam,
            _ => ShowInputKind.UvcWebcam
        };

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
                ResolveProgramResolutionLabel(snapshot));
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

    private void RefreshSceneItems()
    {
        SceneItems = Scenes
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
        OnPropertyChanged(nameof(SceneRailSummary));
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
        var workingRoutes = mutableRoutes.Select(route => route.Clone()).ToList();

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
        }
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
                PreviewCanvasLayers[index].SyncFromRoute(RoomVideoParticipants);
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
                OnPreviewCanvasLayerChanged));
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
        var participants = SceneRoutingService.GetSceneParticipants(scene, routes, RoomVideoParticipants);
        var tilesByParticipant = MultiviewTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        return participants
            .Select(participant =>
                tilesByParticipant.TryGetValue(participant.Id, out var tile)
                    ? tile
                    : new ParticipantSurfaceTile
                    {
                        Participant = participant,
                        Surface = VideoSurfaceState.Waiting(
                            VideoSurfaceKind.Multiview,
                            $"participant:{participant.Id}",
                            participant.Name)
                    })
            .ToList();
    }

    public void ApplyCanvasPreset(string presetWire)
    {
        var routes = GetMutableRoutes(PreviewSceneId);
        SceneCanvasLayoutService.ApplyPreset(presetWire, routes);
        CommandStatus = $"{PreviewScene.Name} canvas preset applied ({presetWire})";
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

        CommandStatus = $"{PreviewScene.Name} source {layer.LayerIndex + 1} updated on canvas";
        PublishPreviewCompositionState(
            PreviewScene,
            routes.Select(route => route.Clone()).ToList());
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
        _bridge.HealthChanged -= OnBridgeHealthChanged;
        _bridge.StatusChanged -= OnBridgeStatusChanged;
        _bridge.SnapshotChanged -= OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived -= OnZoomVideoFrameReceived;
        _bridge.ProgramFramePreviewReceived -= OnProgramFramePreviewReceived;
        _bridge.ProgramSharedTextureReceived -= _surfaces.OnProgramSharedTexture;
        CaptureDeviceFrameRouter.FrameReceived -= OnCaptureDeviceFrameReceived;
        _surfaces.SurfacesChanged -= RefreshSurfaceBindings;

        ForceShutdownMediaCore();

        _surfaces.Dispose();
        _captureDiscovery.Dispose();
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

    private void OnCaptureDeviceFrameReceived(CaptureDeviceFrame frame) =>
        _surfaces.OnCaptureDeviceFrame(frame);

    private void OnProgramFramePreviewReceived(ProgramFramePreview preview)
    {
        _surfaces.OnProgramFramePreview(preview);
        Settings.ObserveProgramPreviewFrame(preview);
    }
}
