using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.WinUI.Models;

namespace CoreVideoPro.WinUI.ViewModels.MagicScene;

/// <summary>
/// Owns the Magic Scene / Set &amp; Forget / production-mode automation extracted from the
/// StudioViewModel god object (PR1 of the strangler refactor). Move-only, behavior-parity:
/// the property/command/method bodies are the originals, with cross-cluster calls routed
/// through <see cref="IMagicSceneHost"/> instead of <c>this</c>. StudioViewModel exposes the
/// same bound properties/commands via thin same-named forwarders so XAML x:Bind is unchanged.
///
/// Threading: the automation timer stays a UI-thread <c>DispatcherQueueTimer</c> (supplied by
/// the host) — <see cref="EvaluateAutomationPolicy"/> runs on the UI thread exactly as before,
/// so the 0xc000027b churn/reentrancy rules are preserved. The DECISION logic still lives in
/// the external <c>ProductionStateHelper</c>/<c>AutoProductionState</c> (fed via
/// <see cref="Recommendation"/> from the snapshot-apply path); this type only sequences it.
/// </summary>
public sealed partial class MagicSceneCoordinator : ObservableObject
{
    private readonly IMagicSceneHost _host;
    private readonly IAutomationTimer _timer;

    private string? _pendingSceneId;
    private DateTimeOffset? _pendingSince;
    private bool _takeInFlight;

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
    private double _automationConfidenceThreshold = 70;

    [ObservableProperty]
    private double _automationSwitchDelaySeconds = 4;

    [ObservableProperty]
    private double _automationPanelParticipantThreshold = 4;

    [ObservableProperty]
    private string _automationLastAction = "Automation is idle";

    /// <summary>
    /// The current scene recommendation. Fed by the snapshot-apply path
    /// (StudioViewModel.RefreshProductionReadouts) — a plain property (no change
    /// notification), mirroring the original private <c>_automationRecommendation</c> field.
    /// </summary>
    public AutoProductionState Recommendation { get; set; } =
        ProductionStateHelper.BuildAutomationRecommendation([], ProductionCatalog.Scenes);

    public MagicSceneCoordinator(IMagicSceneHost host)
    {
        _host = host;
        _timer = host.CreateAutomationTimer(EvaluateAutomationPolicy);
    }

    private string RecommendedSceneName =>
        ProductionStateHelper.RecommendedSceneName(_host.Scenes, Recommendation.RecommendedSceneId);

    /// <summary>Stops the automation timer. Called from StudioViewModel disposal.</summary>
    public void Stop() => _timer.Stop();

    [RelayCommand]
    private void ToggleAutomation()
    {
        if (!CanToggleSetAndForget() && ProductionMode != ProductionMode.SetAndForget)
        {
            _host.CommandStatus = "Set & Forget is locked on this license tier";
            return;
        }

        ProductionMode = ProductionMode == ProductionMode.SetAndForget
            ? ProductionMode.Manual
            : ProductionMode.SetAndForget;
        AutomationButtonLabel = ProductionMode == ProductionMode.SetAndForget
            ? "Automation enabled"
            : "Automation disabled";
        _host.RefreshProductionReadouts();
        _host.CommandStatus = ProductionMode == ProductionMode.SetAndForget
            ? $"Set & Forget enabled: {Recommendation.Reason}"
            : "Manual mode — operator controls scenes";
        RefreshTransportAutomationState();
    }

    [RelayCommand]
    private void RunMagicScene()
    {
        if (!_host.IsInMeeting)
        {
            _host.CommandStatus = "Magic Scene requires an active Zoom meeting";
            return;
        }

        var recommendedSceneId = Recommendation.RecommendedSceneId;
        _host.PreviewSceneId = recommendedSceneId;
        var sceneName = RecommendedSceneName;
        MagicSceneStatus = $"Magic Scene applied: {sceneName} queued on preview";
        ApplyAutomationOverlayPolicy();
        _host.SchedulePreviewRoutingRefresh();
        _host.CommandStatus = $"{sceneName} queued by Magic Scene";
        _host.RefreshSceneItems();
    }

    public void EvaluateAutomationPolicy()
    {
        if (ProductionMode != ProductionMode.SetAndForget)
        {
            if (_timer.IsRunning)
            {
                _timer.Stop();
            }

            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = "Manual mode - automation is not changing scenes";
            return;
        }

        if (!_timer.IsRunning)
        {
            _timer.Start();
        }

        ApplyAutomationOverlayPolicy();

        if (_host.RoomVideoParticipantCount == 0)
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = "Waiting for meeting participants";
            return;
        }

        if (Recommendation.Confidence < AutomationConfidenceThreshold)
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = $"Holding current scene - confidence {Recommendation.Confidence}% is below {AutomationConfidenceThreshold:0}%";
            return;
        }

        var targetSceneId = Recommendation.RecommendedSceneId;
        if (string.Equals(targetSceneId, _host.ActiveSceneId, StringComparison.Ordinal))
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = $"{RecommendedSceneName} is already on program";
            return;
        }

        var now = DateTimeOffset.UtcNow;
        if (!string.Equals(_pendingSceneId, targetSceneId, StringComparison.Ordinal))
        {
            _pendingSceneId = targetSceneId;
            _pendingSince = now;
            AutomationLastAction = $"Holding {RecommendedSceneName} for {AutomationSwitchDelaySeconds:0}s before switching";
            return;
        }

        var elapsedSeconds = _pendingSince is { } pendingSince
            ? (now - pendingSince).TotalSeconds
            : 0;
        if (elapsedSeconds < AutomationSwitchDelaySeconds)
        {
            AutomationLastAction = $"Holding {RecommendedSceneName}: {elapsedSeconds:0.0}/{AutomationSwitchDelaySeconds:0}s";
            return;
        }

        if (!string.Equals(_host.PreviewSceneId, targetSceneId, StringComparison.Ordinal))
        {
            _host.PreviewSceneId = targetSceneId;
            _host.SchedulePreviewRoutingRefresh();
        }

        if (AutomationAutoTakeEnabled)
        {
            AutomationLastAction = $"Taking {RecommendedSceneName} to program";
            _ = TakeAutomationPreviewAsync(targetSceneId);
        }
        else
        {
            AutomationLastAction = $"{RecommendedSceneName} queued on preview";
            _host.CommandStatus = AutomationLastAction;
        }
    }

    private async Task TakeAutomationPreviewAsync(string targetSceneId)
    {
        if (_takeInFlight || !string.Equals(_host.PreviewSceneId, targetSceneId, StringComparison.Ordinal) || !_host.CanTake)
        {
            return;
        }

        _takeInFlight = true;
        try
        {
            await _host.TakeAsync();
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = $"{_host.Scenes.First(scene => scene.Id == targetSceneId).Name} taken by automation";
        }
        finally
        {
            _takeInFlight = false;
        }
    }

    private void ApplyAutomationOverlayPolicy()
    {
        var changed = false;
        changed |= _host.SetAutomationGraphic("lower-third", AutomationLowerThirdsEnabled);
        changed |= _host.SetAutomationGraphic("caption", AutomationCaptionsEnabled);

        _host.RefreshProgramLowerThirdKeyPosition();

        if (!AutomationCaptionsEnabled)
        {
            if (!string.IsNullOrEmpty(_host.CaptionText))
            {
                _host.CaptionText = string.Empty;
                changed = true;
            }

            if (!string.IsNullOrEmpty(_host.CaptionSpeaker))
            {
                _host.CaptionSpeaker = string.Empty;
                changed = true;
            }
        }

        if (changed)
        {
            _host.NotifyEnabledGraphicsChanged();
            _host.RequestMediaCoreSync();
        }
    }

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
        _host.CommandStatus = "Automation defaults restored";
    }

    public string BuildAutoProductionReadout()
    {
        var auto = Recommendation;
        return auto.Confidence > 0
            ? $"Auto: {auto.Action} {auto.Confidence}% — {auto.Reason}"
            : auto.Reason;
    }

    public void RefreshTransportAutomationState()
    {
        var canToggle = CanToggleSetAndForget() || ProductionMode == ProductionMode.SetAndForget;
        _host.ApplyTransportAutomationState(ProductionMode, canToggle);
    }

    private bool CanToggleSetAndForget() => _host.IsSetAndForgetEntitled();

    partial void OnProductionModeChanged(ProductionMode value)
    {
        RefreshTransportAutomationState();
        EvaluateAutomationPolicy();
        OnPropertyChanged(nameof(AutomationButtonLabel));
        OnPropertyChanged(nameof(AutoProductionReadout));
    }

    partial void OnAutomationAutoTakeEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationPreferScreenShareChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationLowerThirdsEnabledChanged(bool value) => OnAutomationPolicyChanged();

    partial void OnAutomationAutoAssignInputsEnabledChanged(bool value)
    {
        OnAutomationPolicyChanged();
        // Turning it on should immediately fill free slots from the current roster.
        _host.ReapplyShowInputAutoAssign();
    }

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
        _pendingSceneId = null;
        _pendingSince = null;
        _host.NotifyAutomationPolicySummaries();
        _host.RefreshProgramLowerThirdKeyPosition();
        _host.RefreshProductionReadouts();
    }
}
