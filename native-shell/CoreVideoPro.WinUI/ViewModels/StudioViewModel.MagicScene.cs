using System.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels.MagicScene;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// StudioViewModel's Magic Scene / Set &amp; Forget façade (PR1 strangler). The automation
/// state + policy engine live in <see cref="MagicSceneCoordinator"/>; StudioViewModel keeps
/// same-named forwarder properties/commands so XAML x:Bind paths (<c>ViewModel.ProductionMode</c>,
/// etc.) are unchanged, and implements <see cref="IMagicSceneHost"/> over <c>this</c> (the
/// Overlays/Transport sub-VM pattern). A PropertyChanged bridge re-raises the coordinator's
/// notifications on the VM so bindings update exactly as they did when these were
/// StudioViewModel [ObservableProperty] fields.
/// </summary>
public sealed partial class StudioViewModel : IMagicSceneHost
{
    public MagicSceneCoordinator MagicScene { get; }

    private void OnMagicScenePropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is { } name)
        {
            OnPropertyChanged(name);
        }
    }

    // --- bound forwarders: same names as the former [ObservableProperty] fields so XAML is unchanged ---
    public ProductionMode ProductionMode
    {
        get => MagicScene.ProductionMode;
        set => MagicScene.ProductionMode = value;
    }

    public string MagicSceneStatus
    {
        get => MagicScene.MagicSceneStatus;
        set => MagicScene.MagicSceneStatus = value;
    }

    public string AutoProductionReadout
    {
        get => MagicScene.AutoProductionReadout;
        set => MagicScene.AutoProductionReadout = value;
    }

    public string AutomationButtonLabel
    {
        get => MagicScene.AutomationButtonLabel;
        set => MagicScene.AutomationButtonLabel = value;
    }

    public bool AutomationAutoTakeEnabled
    {
        get => MagicScene.AutomationAutoTakeEnabled;
        set => MagicScene.AutomationAutoTakeEnabled = value;
    }

    public bool AutomationPreferScreenShare
    {
        get => MagicScene.AutomationPreferScreenShare;
        set => MagicScene.AutomationPreferScreenShare = value;
    }

    public bool AutomationLowerThirdsEnabled
    {
        get => MagicScene.AutomationLowerThirdsEnabled;
        set => MagicScene.AutomationLowerThirdsEnabled = value;
    }

    public bool AutomationAutoAssignInputsEnabled
    {
        get => MagicScene.AutomationAutoAssignInputsEnabled;
        set => MagicScene.AutomationAutoAssignInputsEnabled = value;
    }

    public bool AutomationCaptionsEnabled
    {
        get => MagicScene.AutomationCaptionsEnabled;
        set => MagicScene.AutomationCaptionsEnabled = value;
    }

    public double AutomationConfidenceThreshold
    {
        get => MagicScene.AutomationConfidenceThreshold;
        set => MagicScene.AutomationConfidenceThreshold = value;
    }

    public double AutomationSwitchDelaySeconds
    {
        get => MagicScene.AutomationSwitchDelaySeconds;
        set => MagicScene.AutomationSwitchDelaySeconds = value;
    }

    public double AutomationPanelParticipantThreshold
    {
        get => MagicScene.AutomationPanelParticipantThreshold;
        set => MagicScene.AutomationPanelParticipantThreshold = value;
    }

    public string AutomationLastAction
    {
        get => MagicScene.AutomationLastAction;
        set => MagicScene.AutomationLastAction = value;
    }

    // --- command forwarders (XAML binds ViewModel.<Name>Command) ---
    public IRelayCommand ToggleAutomationCommand => MagicScene.ToggleAutomationCommand;

    public IRelayCommand RunMagicSceneCommand => MagicScene.RunMagicSceneCommand;

    public IRelayCommand ResetAutomationDefaultsCommand => MagicScene.ResetAutomationDefaultsCommand;

    // --- IMagicSceneHost (explicit: the coordinator is the only caller; VM keeps its own API) ---
    int IMagicSceneHost.RoomVideoParticipantCount => RoomVideoParticipants.Count;

    IReadOnlyList<Scene> IMagicSceneHost.Scenes => Scenes;

    string IMagicSceneHost.ActiveSceneId => ActiveSceneId;

    string IMagicSceneHost.PreviewSceneId
    {
        get => PreviewSceneId;
        set => PreviewSceneId = value;
    }

    bool IMagicSceneHost.CanTake => CanTake;

    bool IMagicSceneHost.IsInMeeting => Settings.IsInMeeting;

    bool IMagicSceneHost.IsMediaCoreRunning => _bridge.Running;

    string IMagicSceneHost.CommandStatus
    {
        set => CommandStatus = value;
    }

    Task IMagicSceneHost.TakeAsync() => _transportCoordinator.TakeAsync();

    void IMagicSceneHost.SchedulePreviewRoutingRefresh() => SchedulePreviewRoutingRefresh();

    void IMagicSceneHost.RefreshSceneItems() => RefreshSceneItems();

    void IMagicSceneHost.RefreshProductionReadouts() => RefreshProductionReadouts();

    void IMagicSceneHost.ApplyTransportAutomationState(ProductionMode mode, bool canToggle) =>
        Transport.ApplyAutomationState(mode, canToggle);

    bool IMagicSceneHost.IsSetAndForgetEntitled()
    {
        var entitlements = LicenseCatalog.DeriveEntitlements(
            new LicenseState { Tier = Settings.LicenseTier, Status = Settings.LicenseStatus },
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        return entitlements.SetAndForget;
    }

    bool IMagicSceneHost.SetAutomationGraphic(string kind, bool enabled) => SetAutomationGraphic(kind, enabled);

    void IMagicSceneHost.RefreshProgramLowerThirdKeyPosition() => RefreshProgramLowerThirdKeyPosition();

    string IMagicSceneHost.CaptionText
    {
        get => CaptionText;
        set => CaptionText = value;
    }

    string IMagicSceneHost.CaptionSpeaker
    {
        get => CaptionSpeaker;
        set => CaptionSpeaker = value;
    }

    void IMagicSceneHost.NotifyEnabledGraphicsChanged() => OnPropertyChanged(nameof(EnabledGraphics));

    void IMagicSceneHost.RequestMediaCoreSync() => _ = TrySyncMediaCoreAsync();

    void IMagicSceneHost.ReapplyShowInputAutoAssign() => ReapplyShowInputAutoAssign();

    void IMagicSceneHost.NotifyAutomationPolicySummaries()
    {
        OnPropertyChanged(nameof(AutomationPolicySummary));
        OnPropertyChanged(nameof(AutomationScenePolicySummary));
        OnPropertyChanged(nameof(AutomationOverlayPolicySummary));
        OnPropertyChanged(nameof(AutomationTakeModeLabel));
    }

    IAutomationTimer IMagicSceneHost.CreateAutomationTimer(Action onTick)
    {
        var timer = _dispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(500);
        timer.Tick += (_, _) => onTick();
        return new DispatcherAutomationTimer(timer);
    }

    private sealed class DispatcherAutomationTimer(DispatcherQueueTimer timer) : IAutomationTimer
    {
        public bool IsRunning => timer.IsRunning;

        public void Start() => timer.Start();

        public void Stop() => timer.Stop();
    }
}
