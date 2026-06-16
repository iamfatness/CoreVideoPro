using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel : ObservableObject, IAsyncDisposable
{
    private readonly MediaCoreBridgeService _bridge = new();
    private readonly VideoSurfaceCoordinator _surfaces = new();
    private readonly ZoomOAuthService _zoomOAuth;
    private readonly ZoomOAuthAppCoordinator _zoomOAuthCoordinator;
    private readonly string _currentRoomId;
    private readonly string _currentRoomName;

    [ObservableProperty]
    private bool _engineRunning;

    [ObservableProperty]
    private string _engineStatus = string.Empty;

    [ObservableProperty]
    private string _zoomStatus = "Zoom Connected";

    [ObservableProperty]
    private StudioTab _activeTab = StudioTab.Studio;

    [ObservableProperty]
    private StudioViewMode _viewMode = StudioViewMode.Program;

    [ObservableProperty]
    private string _activeSceneId = "speaker-slides";

    [ObservableProperty]
    private string _previewSceneId = "speaker-slides";

    [ObservableProperty]
    private string? _selectedParticipantId = "p2";

    [ObservableProperty]
    private bool _recording;

    [ObservableProperty]
    private bool _streaming;

    [ObservableProperty]
    private string _outputStatus = LiveProductionSync.DemoDefaults.OutputStatus;

    [ObservableProperty]
    private string _outputSessionStatus = LiveProductionSync.DemoDefaults.OutputStatus;

    [ObservableProperty]
    private string _commandStatus = "Program ready";

    [ObservableProperty]
    private string _captionSpeaker = LiveProductionSync.DemoDefaults.CaptionSpeaker;

    [ObservableProperty]
    private string _captionText = LiveProductionSync.DemoDefaults.CaptionText;

    [ObservableProperty]
    private string _lowerThirdName = LiveProductionSync.DemoDefaults.LowerThirdName;

    [ObservableProperty]
    private string _lowerThirdTitle = LiveProductionSync.DemoDefaults.LowerThirdTitle;

    [ObservableProperty]
    private string _lowerThirdOrg = LiveProductionSync.DemoDefaults.LowerThirdOrg;

    [ObservableProperty]
    private VideoSurfaceState _programSurface = VideoSurfaceState.Waiting(VideoSurfaceKind.Program, "program", "Program");

    [ObservableProperty]
    private VideoSurfaceState _previewSurface = VideoSurfaceState.Waiting(VideoSurfaceKind.Preview, "preview", "Preview");

    [ObservableProperty]
    private IReadOnlyList<ParticipantSurfaceTile> _multiviewTiles = [];

    [ObservableProperty]
    private ProductionMode _productionMode = DemoProduction.Mode;

    [ObservableProperty]
    private string _magicSceneStatus = DemoProduction.MagicSceneStatus;

    [ObservableProperty]
    private string _autoProductionReadout = BuildAutoProductionReadout();

    [ObservableProperty]
    private string _automationButtonLabel = "Automation enabled";

    [ObservableProperty]
    private ColorGrade _colorGrade = DemoProduction.ColorGrade;

    [ObservableProperty]
    private string _mediaBinSummary = DemoProduction.MediaBinSummary;

    [ObservableProperty]
    private string _captureFleetSummary = DemoProduction.CaptureFleetSummary;

    [ObservableProperty]
    private bool _dualCaptureLive = DemoProduction.DualCaptureLive;

    [ObservableProperty]
    private string _feedHealthSummary = DemoProduction.FeedHealthSummary;

    [ObservableProperty]
    private string _previewSceneParticipants = "David Chen + screen share";

    private readonly Dictionary<string, List<SourceRoute>> _sceneRoutes = new(StringComparer.Ordinal);

    public SettingsViewModel Settings { get; }

    public TransportViewModel Transport { get; }

    public OverlaysViewModel Overlays { get; }

    public ObservableCollection<GraphicOverlay> Graphics { get; } =
        new(DemoProduction.Graphics);

    public ObservableCollection<CaptureDevice> CaptureDevices { get; } =
        new(DemoProduction.CaptureDevices);

    public IReadOnlyList<FeedHealthRow> FeedHealthRows { get; } = DemoProduction.FeedHealthRows();

    public BrandKit BrandKit { get; } = DemoProduction.BrandKit;

    public IReadOnlyList<CaptionTranscriptEntry> CaptionTranscript { get; } = DemoProduction.CaptionTranscript;

    public AudioMixState AudioMix { get; } = DemoProduction.AudioMix;

    public IReadOnlyList<MediaBinGroup> MediaBinGroups { get; } = DemoProduction.MediaBinGroups();

    public IReadOnlyList<AudioParticipantRow> AudioParticipantRows { get; private set; } = [];

    public string CaptionQualitySummary => "Avg confidence 94% · tier excellent · visibility live";

    public ObservableCollection<SlotEditorItemViewModel> PreviewSlotEditors { get; } = [];

    public IReadOnlyList<string> PreviewRouteWarnings { get; private set; } = [];

    public bool HasPreviewRouteWarnings => PreviewRouteWarnings.Count > 0;

    public IReadOnlyList<ParticipantSurfaceTile> PreviewSceneTiles { get; private set; } = [];

    public string PreviewSceneSummary => PreviewScene.Name;

    public string LoudnessTargetLabel => "target -16 LUFS";

    public string LoudnessLevelLabel => "on target";

    public string TruePeakLabel => "-6.0 dBTP";

    public string GainAdjustLabel => "+0.0 dB";

    public string ClipTrimSummary => "00:00:12:00 – 00:01:45:00 · 1:33 duration";

    public string ChapterSummary => "3 chapters · next: Product roadmap";

    public string SceneIntelligenceSummary => DemoProduction.SceneIntelligenceSummary;

    public string RecommendedSceneName => DemoProduction.RecommendedSceneName;

    public string RecommendedLayout => "speaker-slides";

    public string RecommendedConfidence => $"{DemoProduction.AutoProduction.Confidence}%";

    public string AutoSwitchLabel => "Stable";

    public int CamerasOnCount => RoomVideoParticipants.Count;

    public string ScreenShareLabel => RoomVideoParticipants.Any(p => p.IsScreenSharing) ? "Active" : "Off";

    public string AutoProductionReason => DemoProduction.AutoProduction.Reason;

    public StudioViewModel()
    {
        var room = DemoProduction.CurrentRoom();
        _currentRoomId = room.Id;
        _currentRoomName = room.Name;

        Scenes = DemoProduction.Scenes;
        RoomVideoParticipants = DemoProduction.VideoParticipantsInRoom(_currentRoomId);
        CurrentRoomLabel = _currentRoomName;
        _multiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
        RefreshParticipantListItems();
        RefreshSceneItems();
        RefreshAudioParticipantRows();

        _zoomOAuth = new ZoomOAuthService(new FileZoomTokenStore(FileZoomTokenStore.DefaultTokenStorePath()));
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
            () => EngineRunning,
            _zoomOAuth,
            status => ZoomStatus = status);
        _zoomOAuthCoordinator.SetStatusChangedHandler(message =>
        {
            Settings.OauthStatusMessage = message;
            _ = Settings.RefreshOAuthStatusAsync();
        });
        Transport = new TransportViewModel();
        Overlays = new OverlaysViewModel(this);
        InitializeSceneRoutes();
        RefreshPreviewRoutingState();
        RefreshTransportState();

        _bridge.HealthChanged += OnBridgeHealthChanged;
        _bridge.StatusChanged += status => EngineStatus = status;
        _bridge.SnapshotChanged += OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived += _surfaces.OnZoomVideoFrame;
        _bridge.ProgramFramePreviewReceived += _surfaces.OnProgramFramePreview;
        _bridge.ProgramSharedTextureReceived += _surfaces.OnProgramSharedTexture;
        _surfaces.SurfacesChanged += RefreshSurfaceBindings;
    }

    public IReadOnlyList<Scene> Scenes { get; }

    public IReadOnlyList<SceneDisplayItem> SceneItems { get; private set; } = [];

    public IReadOnlyList<Participant> RoomVideoParticipants { get; private set; }

    public IReadOnlyList<Participant> SceneParticipants => RoomVideoParticipants;

    public Scene ProgramScene => Scenes.First(s => s.Id == ActiveSceneId);

    public Scene PreviewScene => Scenes.First(s => s.Id == PreviewSceneId);

    [ObservableProperty]
    private string _currentRoomLabel;

    public string EngineRunningLabel => EngineRunning ? "Engine On" : "Engine Off";

    public string RecordingLabel => Recording ? "Recording" : "Record";

    public string StreamingLabel => Streaming ? "Streaming" : "Stream";

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
        Background = new SolidColorBrush(Windows.UI.Color.FromArgb(0, 0, 0, 0)),
        BorderBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 42, 52, 60)),
        Foreground = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 148, 165, 155))
    };

    public TabChrome StudioTabChrome => ActiveTab == StudioTab.Studio ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome SettingsTabChrome => ActiveTab == StudioTab.Settings ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome SourcesTabChrome => ActiveTab == StudioTab.Sources ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome OverlaysTabChrome => ActiveTab == StudioTab.Overlays ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome AudioTabChrome => ActiveTab == StudioTab.Audio ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome MediaTabChrome => ActiveTab == StudioTab.Media ? SelectedTabChrome : DefaultTabChrome;

    public TabChrome AutomationTabChrome => ActiveTab == StudioTab.Automation ? SelectedTabChrome : DefaultTabChrome;

    public Participant? SelectedParticipant =>
        SelectedParticipantId is not null
            ? DemoProduction.Participants.FirstOrDefault(p => p.Id == SelectedParticipantId)
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

    public Brush EngineToggleBackground => EngineRunning
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(31, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(10, 255, 255, 255));

    public Brush EngineToggleBorder => EngineRunning
        ? new SolidColorBrush(Windows.UI.Color.FromArgb(140, 61, 220, 151))
        : new SolidColorBrush(Windows.UI.Color.FromArgb(46, 189, 207, 196));

    public Brush EngineToggleForeground => EngineRunning
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

    public bool ShowMultiview => ViewMode == StudioViewMode.Multiview;

    partial void OnViewModeChanged(StudioViewMode value)
    {
        OnPropertyChanged(nameof(ViewModeLabel));
        OnPropertyChanged(nameof(ShowProgram));
        OnPropertyChanged(nameof(ShowPreview));
        OnPropertyChanged(nameof(ShowMultiview));
    }

    partial void OnEngineRunningChanged(bool value)
    {
        OnPropertyChanged(nameof(EngineRunningLabel));
        OnPropertyChanged(nameof(EngineToggleBackground));
        OnPropertyChanged(nameof(EngineToggleBorder));
        OnPropertyChanged(nameof(EngineToggleForeground));
        RefreshRoomParticipants();
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
        OnPropertyChanged(nameof(IsOverlaysTab));
        OnPropertyChanged(nameof(IsAudioTab));
        OnPropertyChanged(nameof(IsMediaTab));
        OnPropertyChanged(nameof(IsAutomationTab));
        OnPropertyChanged(nameof(ActiveTabKey));
        OnPropertyChanged(nameof(StudioTabChrome));
        OnPropertyChanged(nameof(SettingsTabChrome));
        OnPropertyChanged(nameof(SourcesTabChrome));
        OnPropertyChanged(nameof(OverlaysTabChrome));
        OnPropertyChanged(nameof(AudioTabChrome));
        OnPropertyChanged(nameof(MediaTabChrome));
        OnPropertyChanged(nameof(AutomationTabChrome));
    }

    partial void OnActiveSceneIdChanged(string value)
    {
        RefreshSceneItems();
        OnPropertyChanged(nameof(ProgramScene));
    }

    partial void OnPreviewSceneIdChanged(string value)
    {
        RefreshSceneItems();
        OnPropertyChanged(nameof(PreviewScene));
        OnPropertyChanged(nameof(PreviewSceneSummary));
        RefreshPreviewRoutingState();
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

    [RelayCommand]
    private async Task ToggleEngineAsync()
    {
        try
        {
            if (EngineRunning)
            {
                StopEngine("Engine off — Zoom ingest paused");
            }
            else
            {
                EngineStatus = "Starting engine…";
                await _bridge.StartAsync().ConfigureAwait(false);
                _bridge.ConfigureZoomSpineSync(BuildSpinePayload);
                EngineRunning = true;
                _surfaces.SetEngineRunning(true, _bridge.Profile?.Renderer);
                _surfaces.SetPreviewParticipant(SelectedParticipantId);
                EngineStatus = _bridge.ProfileSummary;
                ZoomStatus = "Zoom Connected";
                Settings.RefreshSdkReadiness();
                await SyncActiveSceneAsync().ConfigureAwait(false);
                RefreshSurfaceBindings();
                RefreshTransportState();
            }
        }
        catch (Exception ex)
        {
            EngineRunning = false;
            _surfaces.SetEngineRunning(false);
            EngineStatus = ex.Message;
            Settings.RefreshSdkReadiness();
            RefreshSurfaceBindings();
            RefreshTransportState();
        }
    }

    [RelayCommand]
    private void SelectScene(string sceneId)
    {
        PreviewSceneId = sceneId;
        CommandStatus = $"{Scenes.First(s => s.Id == sceneId).Name} queued on preview";
        RefreshPreviewRoutingState();
    }

    [RelayCommand]
    private async Task TakeAsync()
    {
        ActiveSceneId = PreviewSceneId;
        CopyPreviewRoutesToScene(ActiveSceneId);
        var scene = Scenes.First(s => s.Id == ActiveSceneId);
        CommandStatus = $"{scene.Name} taken with fade";
        OutputStatus = "Program updated";

        if (EngineRunning)
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

    [RelayCommand]
    private async Task ToggleRecordingAsync()
    {
        Recording = !Recording;
        OutputStatus = Recording ? "Recording to Q2_Product_Update.mp4" : "Recording stopped";

        if (EngineRunning)
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
    private async Task ToggleStreamingAsync()
    {
        Streaming = !Streaming;
        OutputStatus = Streaming ? "Streaming to YouTube + Custom RTMP" : "Streaming stopped";

        if (EngineRunning)
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
    private void SelectTab(string tab)
    {
        ActiveTab = tab switch
        {
            "settings" => StudioTab.Settings,
            "sources" => StudioTab.Sources,
            "overlays" => StudioTab.Overlays,
            "audio" => StudioTab.Audio,
            "media" => StudioTab.Media,
            "automation" => StudioTab.Automation,
            _ => StudioTab.Studio
        };
    }

    [RelayCommand]
    private void SelectParticipant(string participantId)
    {
        SelectedParticipantId = participantId;
    }

    [RelayCommand]
    private void ToggleGraphic(string graphicId)
    {
        var graphic = Graphics.FirstOrDefault(g => g.Id == graphicId);
        if (graphic is null)
        {
            return;
        }

        graphic.Enabled = !graphic.Enabled;
        CommandStatus = $"{graphic.Name} {(graphic.Enabled ? "enabled" : "disabled")} on program";
        OnPropertyChanged(nameof(EnabledGraphics));
    }

    public IReadOnlyList<GraphicOverlay> EnabledGraphics =>
        Graphics.Where(graphic => graphic.Enabled).ToList();

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
        DemoProduction.Mode = ProductionMode;
        AutomationButtonLabel = ProductionMode == ProductionMode.SetAndForget
            ? "Automation enabled"
            : "Automation disabled";
        AutoProductionReadout = BuildAutoProductionReadout();
        CommandStatus = ProductionMode == ProductionMode.SetAndForget
            ? $"Set & Forget enabled: {DemoProduction.AutoProduction.Reason}"
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

        var recommendedSceneId = DemoProduction.AutoProduction.RecommendedSceneId;
        PreviewSceneId = recommendedSceneId;
        var sceneName = DemoProduction.RecommendedSceneName;
        MagicSceneStatus = $"Magic Scene applied: {sceneName} queued on preview";
        CommandStatus = $"{sceneName} queued by Magic Scene";
        RefreshSceneItems();
    }

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
        RefreshCaptureFleetSummary();
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
        CaptureFleetSummary =
            $"{CaptureDevices.Count(d => d.ConnectionState == CaptureConnectionState.Connected)} connected · " +
            $"{CaptureDevices.Count(d => d.ConnectionState == CaptureConnectionState.Detected)} detected";
        DualCaptureLive = CaptureDevices.Count(d => d.ConnectionState == CaptureConnectionState.Connected) >= 2;
    }

    private static string BuildAutoProductionReadout()
    {
        var auto = DemoProduction.AutoProduction;
        return $"Auto: {auto.Action} {auto.Confidence}% - {auto.Reason}";
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
                EngineRunning = EngineRunning,
                OAuthSignedIn = Settings.ZoomOAuthSignedIn,
                SdkVersion = _bridge.Profile?.Name ?? "zoom-engine",
                SdkRuntimeReady = Settings.SdkIsReady || EngineRunning
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
                route.ParticipantId))
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

        return new MediaCoreProductionSyncContext
        {
            ActiveSceneId = ActiveSceneId,
            SceneRoutes = sceneRoutes,
            Participants = participants,
            Recording = Recording,
            Streaming = Streaming,
            StreamDestinations = ["rtmp", "ndi"],
            RecordingTargets = MediaCoreProductionSyncContext.DefaultRecordingTargets with
            {
                IsoParticipantIds = isoParticipantIds
            },
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
                BrandKit.BrandColor,
                BrandKit.AccentColor,
                BrandKit.BackgroundColor,
                BrandKit.FontFamily,
                BrandKit.LowerThirdStyle),
            AudioMixChannels = audioChannels,
            CaptionText = CaptionText,
            CaptionSpeaker = CaptionSpeaker
        };
    }

    private static string MapParticipantHealth(FeedHealth health) => health switch
    {
        FeedHealth.LowResolution => "low-resolution",
        FeedHealth.Recovering => "recovering",
        FeedHealth.VideoOff => "video-off",
        _ => "live"
    };

    private void OnBridgeHealthChanged(MediaCoreHealth health)
    {
        if (health.Stopped)
        {
            _surfaces.SetEngineRunning(false);
            EngineRunning = false;
            RestoreDemoLiveProductionState();
            RefreshSurfaceBindings();
        }
        else if (health.Recovering)
        {
            EngineStatus = $"Media core recovering (restart {health.RestartCount})";
        }
    }

    private void OnSnapshotChanged(NativeMediaCoreStateSnapshot snapshot)
    {
        _surfaces.OnMediaCoreSnapshot(snapshot);

        if (!EngineRunning)
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

        ApplyLiveProductionPatch(LiveProductionSync.MapSnapshotToStudioPatch(snapshot, liveProductionContext));
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

    private void StopEngine(string status)
    {
        _bridge.ConfigureZoomSpineSync(null);
        _bridge.Stop();
        _surfaces.SetEngineRunning(false);
        EngineRunning = false;
        EngineStatus = status;
        CommandStatus = status;
        RestoreDemoLiveProductionState();
        Settings.RefreshSdkReadiness();
        RefreshSurfaceBindings();
        RefreshTransportState();
    }

    private void StopEngineForBreakoutRoomChange(string status) => StopEngine(status);

    private void ApplyLiveProductionPatch(LiveProductionSync.StudioLiveProductionPatch patch)
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
        else if (patch.Participants is { Count: 0 })
        {
            RestoreDemoLiveProductionParticipants();
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
        OnPropertyChanged(nameof(CamerasOnCount));
        OnPropertyChanged(nameof(ScreenShareLabel));
        RefreshPreviewRoutingState();
    }

    private void RestoreDemoLiveProductionParticipants() => RefreshRoomParticipants();

    private void RestoreDemoLiveProductionState()
    {
        ApplyLiveProductionPatch(LiveProductionSync.CreateDemoFallbackPatch());
        RefreshTransportState();
    }

    private void RefreshRoomParticipants()
    {
        if (EngineRunning && _bridge.LastSnapshot is { } snapshot)
        {
            var liveParticipants = LiveProductionSync.MapSnapshotParticipants(snapshot);
            if (liveParticipants is { Count: > 0 })
            {
                ApplyLiveParticipants(liveParticipants);
                return;
            }
        }

        RoomVideoParticipants = DemoProduction.VideoParticipantsInRoom(_currentRoomId);
        CurrentRoomLabel = EngineRunning
            ? _currentRoomName
            : $"{_currentRoomName} — engine off, feeds paused";
        MultiviewTiles = _surfaces.BuildMultiviewTiles(RoomVideoParticipants);
        OnPropertyChanged(nameof(RoomVideoParticipants));
        OnPropertyChanged(nameof(CurrentRoomHeader));
        RefreshParticipantListItems();
        RefreshAudioParticipantRows();
        OnPropertyChanged(nameof(CamerasOnCount));
        OnPropertyChanged(nameof(ScreenShareLabel));
        RefreshPreviewRoutingState();
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
        AudioParticipantRows = RoomVideoParticipants
            .Select(participant =>
            {
                var mix = AudioMix.Participants.FirstOrDefault(m => m.ParticipantId == participant.Id);
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
    }

    private void RefreshTransportState()
    {
        RefreshTransportAutomationState();
        Transport.ApplyMeetingState(Settings.IsInMeeting);

        if (EngineRunning && _bridge.LastSnapshot is { } snapshot)
        {
            Transport.ApplySnapshot(
                snapshot,
                snapshot.Recording?.Active == true,
                LiveProductionSync.IsStreamingLive(snapshot),
                ResolveProgramResolutionLabel(snapshot));
            return;
        }

        Transport.ApplyDemoState(Recording, Streaming, ProgramResolutionLabel);
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

    private void RefreshSceneItems()
    {
        SceneItems = Scenes
            .Select(scene => new SceneDisplayItem
            {
                Scene = scene,
                Id = scene.Id,
                IsOnProgram = scene.Id == ActiveSceneId,
                IsOnPreview = scene.Id == PreviewSceneId,
                SelectCommand = SelectSceneCommand
            })
            .ToList();
        OnPropertyChanged(nameof(SceneItems));
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

    private void RefreshPreviewRoutingState()
    {
        var scene = PreviewScene;
        var routes = SceneRoutingService.GetRouteDefaults(
            scene,
            GetMutableRoutes(scene.Id),
            RoomVideoParticipants);

        GetMutableRoutes(scene.Id).Clear();
        GetMutableRoutes(scene.Id).AddRange(routes.Select(route => route.Clone()));

        PreviewSlotEditors.Clear();
        for (var index = 0; index < routes.Count; index++)
        {
            PreviewSlotEditors.Add(new SlotEditorItemViewModel(
                index,
                routes[index],
                RoomVideoParticipants,
                OnPreviewSlotEditorChanged));
        }

        PreviewSceneParticipants = SceneRoutingService.DescribeRouteAssignments(
            scene,
            routes,
            RoomVideoParticipants);

        PreviewRouteWarnings = SceneRoutingService
            .GetRouteWarnings(scene, routes, RoomVideoParticipants)
            .Take(3)
            .ToList();

        var participants = SceneRoutingService.GetSceneParticipants(scene, routes, RoomVideoParticipants);
        var tilesByParticipant = MultiviewTiles.ToDictionary(tile => tile.Participant.Id, tile => tile);
        PreviewSceneTiles = participants
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

        OnPropertyChanged(nameof(PreviewRouteWarnings));
        OnPropertyChanged(nameof(HasPreviewRouteWarnings));
        OnPropertyChanged(nameof(PreviewSceneTiles));
        OnPropertyChanged(nameof(PreviewSlotEditors));
    }

    private void OnPreviewSlotEditorChanged(SlotEditorItemViewModel editor)
    {
        var routes = GetMutableRoutes(PreviewSceneId);
        if (editor.SlotIndex < 0 || editor.SlotIndex >= routes.Count)
        {
            return;
        }

        editor.ApplyTo(routes[editor.SlotIndex]);
        var normalized = SceneRoutingService.NormalizeRouteUpdate(
            routes[editor.SlotIndex],
            RoomVideoParticipants);
        routes[editor.SlotIndex] = normalized;

        CommandStatus = $"{PreviewScene.Name} route {editor.SlotIndex + 1} updated";
        RefreshPreviewRoutingState();
    }

    private void CopyPreviewRoutesToScene(string sceneId)
    {
        var previewRoutes = GetMutableRoutes(PreviewSceneId)
            .Select(route => route.Clone())
            .ToList();
        GetMutableRoutes(sceneId).Clear();
        GetMutableRoutes(sceneId).AddRange(previewRoutes);
    }

    public async ValueTask DisposeAsync()
    {
        _bridge.HealthChanged -= OnBridgeHealthChanged;
        _bridge.SnapshotChanged -= OnSnapshotChanged;
        _bridge.ZoomVideoFrameReceived -= _surfaces.OnZoomVideoFrame;
        _bridge.ProgramFramePreviewReceived -= _surfaces.OnProgramFramePreview;
        _bridge.ProgramSharedTextureReceived -= _surfaces.OnProgramSharedTexture;
        _surfaces.SurfacesChanged -= RefreshSurfaceBindings;
        _surfaces.Dispose();
        _zoomOAuthCoordinator.Dispose();
        await _bridge.DisposeAsync().ConfigureAwait(false);
    }
}