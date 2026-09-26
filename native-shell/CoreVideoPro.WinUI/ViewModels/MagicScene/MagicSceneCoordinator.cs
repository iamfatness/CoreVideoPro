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
    private readonly Func<DateTimeOffset> _clock;
    private readonly DirectorBindingHoldPolicy _bindingHold = new();

    private string? _pendingSceneId;
    private string? _pendingBindingKey;
    private bool _pendingBindingsApplied;
    private IReadOnlyList<CoreVideoPro.MediaCore.Models.NativeDirectorSlotBinding>? _lastTakenBindings;
    private DateTimeOffset? _pendingSince;
    private bool _takeInFlight;
    private bool _settingPreview;
    private bool _evaluating;
    private bool _overlayPolicyDirty = true;
    private long _modeGeneration;

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

    public MagicSceneCoordinator(IMagicSceneHost host, Func<DateTimeOffset>? clock = null)
    {
        _host = host;
        _clock = clock ?? (() => DateTimeOffset.UtcNow);
        _timer = host.CreateAutomationTimer(EvaluateAutomationPolicy);
    }

    private string RecommendedSceneName =>
        ProductionStateHelper.RecommendedSceneName(_host.Scenes, Recommendation.RecommendedSceneId);

    /// <summary>Stops the automation timer. Called from StudioViewModel disposal.</summary>
    public void Stop()
    {
        ProductionMode = ProductionMode.Manual;
        _timer.Stop();
    }

    public void NotifyPreviewSceneChanged()
    {
        if (!_settingPreview && !_takeInFlight && ProductionMode == ProductionMode.SetAndForget)
        {
            NotifyManualSceneSelection();
        }
    }

    public void NotifyManualSceneSelection()
    {
        if (ProductionMode != ProductionMode.SetAndForget) return;
        ProductionMode = ProductionMode.Manual;
        _host.CommandStatus = "Automation paused � operator selected preview";
    }

    private void CuePreview(string sceneId)
    {
        _settingPreview = true;
        try
        {
            if (!string.Equals(_host.PreviewSceneId, sceneId, StringComparison.Ordinal))
            {
                _host.PreviewSceneId = sceneId;
                _host.SchedulePreviewRoutingRefresh();
            }
        }
        finally { _settingPreview = false; }
    }

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
        if (!_host.Scenes.Any(scene => scene.Id == recommendedSceneId) || Recommendation.Confidence <= 0)
        {
            MagicSceneStatus = $"Magic Scene has no available scene recommendation: {Recommendation.Reason}";
            _host.CommandStatus = MagicSceneStatus;
            return;
        }
        if (Recommendation.SlotBindings.Count == 0)
        {
            MagicSceneStatus = $"Magic Scene has no source to bind on Preview: {Recommendation.Reason}";
            _host.CommandStatus = MagicSceneStatus;
            return;
        }
        ProductionMode = ProductionMode.Manual;
        CuePreview(recommendedSceneId);
        if (!_host.ApplyPreviewBindings(recommendedSceneId, Recommendation.SlotBindings, out var bindingReason))
        {
            MagicSceneStatus = $"Magic Scene could not bind Preview: {bindingReason}";
            _host.CommandStatus = MagicSceneStatus;
            return;
        }
        var sceneName = RecommendedSceneName;
        MagicSceneStatus = $"Magic Scene applied: {sceneName} with {Recommendation.SlotBindings.Count} bound source(s) on Preview";

        _host.CommandStatus = $"{sceneName} queued by Magic Scene";
        _host.RefreshSceneItems();
    }

    public void EvaluateAutomationPolicy()
    {
        if (_evaluating) return;
        _evaluating = true;
        try { EvaluateAutomationPolicyCore(); }
        finally { _evaluating = false; }
    }

    private void EvaluateAutomationPolicyCore()
    {
        if (ProductionMode != ProductionMode.SetAndForget)
        {
            if (_timer.IsRunning)
            {
                _timer.Stop();
            }

            _pendingSceneId = null;
            _pendingBindingKey = null;
            _pendingBindingsApplied = false;
            _pendingSince = null;
            _bindingHold.Reset();
            AutomationLastAction = "Manual mode - automation is not changing scenes";
            return;
        }

        if (!_timer.IsRunning)
        {
            _timer.Start();
        }

        // A pending Take owns preview until its acknowledgment arrives. Snapshot
        // recommendation churn must not change the scene under that transaction.
        if (_takeInFlight) return;

        if (!_host.IsInMeeting || _host.RoomVideoParticipantCount == 0)
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = "Waiting for meeting participants";
            return;
        }

        if (!_host.IsMediaCoreRunning)
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = "Waiting for media core before changing scenes";
            return;
        }

        if (_overlayPolicyDirty)
        {
            _overlayPolicyDirty = false;
            ApplyAutomationOverlayPolicy();
        }

        if (Recommendation.Confidence <= 0 || Recommendation.Confidence < AutomationConfidenceThreshold)
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = $"Holding current scene - confidence {Recommendation.Confidence}% is below {AutomationConfidenceThreshold:0}%";
            return;
        }

        var targetSceneId = Recommendation.RecommendedSceneId;
        if (!_host.Scenes.Any(scene => scene.Id == targetSceneId))
        {
            _pendingSceneId = null;
            _pendingSince = null;
            AutomationLastAction = "Recommended scene is no longer available";
            return;
        }
        if (Recommendation.SlotBindings.Count == 0)
        {
            AutomationLastAction = $"Holding Program: no source to bind on Preview. {Recommendation.Reason}";
            return;
        }
        var now = _clock();
        var bindingDecision = _bindingHold.Evaluate(_host.ActiveSceneId, _lastTakenBindings,
            targetSceneId, Recommendation.SlotBindings, now);
        if (!bindingDecision.Ready)
        {
            _pendingSceneId = null;
            _pendingBindingKey = null;
            _pendingBindingsApplied = false;
            _pendingSince = null;
            AutomationLastAction = $"Holding person bindings: {bindingDecision.ElapsedMs}/{bindingDecision.RequiredMs} ms";
            return;
        }
        var bindingKey = string.Join("|", Recommendation.SlotBindings.OrderBy(binding => binding.SlotIndex)
            .Select(binding => $"{binding.SlotIndex}:{binding.PersonId}:{binding.SourceId}"));
        var sceneHoldSeconds = DirectorBindingHoldPolicy.RequiredSceneHoldSeconds(
            _host.ActiveSceneId, targetSceneId, AutomationSwitchDelaySeconds);
        if (string.Equals(targetSceneId, _host.ActiveSceneId, StringComparison.Ordinal) &&
            _lastTakenBindings is not null && string.Equals(bindingKey,
                string.Join("|", _lastTakenBindings.OrderBy(binding => binding.SlotIndex)
                    .Select(binding => $"{binding.SlotIndex}:{binding.PersonId}:{binding.SourceId}")),
                StringComparison.Ordinal))
        {
            _pendingSceneId = null;
            _pendingBindingKey = null;
            _pendingBindingsApplied = false;
            _pendingSince = null;
            AutomationLastAction = $"{RecommendedSceneName} is already on program";
            return;
        }

        if (!string.Equals(_pendingSceneId, targetSceneId, StringComparison.Ordinal) ||
            !string.Equals(_pendingBindingKey, bindingKey, StringComparison.Ordinal))
        {
            _pendingSceneId = targetSceneId;
            _pendingBindingKey = bindingKey;
            _pendingBindingsApplied = false;
            _pendingSince = now;
            // #478 R5: cue Preview at the START of the hold, not at the Take. Only sources are
            // subscribed now, so a guest who is not already on the wall has no feed until their
            // scene is on a bus; cueing it here lets the whole hold warm the subscription (the
            // spine syncs every 500 ms) instead of cutting Program to a cold source.
            CuePreview(targetSceneId);
            if (!_host.ApplyPreviewBindings(targetSceneId, Recommendation.SlotBindings, out var bindingReason))
            {
                _pendingSceneId = null;
                _pendingBindingKey = null;
                AutomationLastAction = $"Holding Program: Preview binding failed. {bindingReason}";
                return;
            }
            AutomationLastAction = $"Holding {RecommendedSceneName} for {sceneHoldSeconds:0.0}s before switching";
            return;
        }

        var elapsedSeconds = _pendingSince is { } pendingSince
            ? (now - pendingSince).TotalSeconds
            : 0;
        if (elapsedSeconds < sceneHoldSeconds)
        {
            AutomationLastAction = $"Holding {RecommendedSceneName}: {elapsedSeconds:0.0}/{sceneHoldSeconds:0.0}s";
            return;
        }

        if (_pendingBindingsApplied && (!AutomationAutoTakeEnabled || Recommendation.Confidence < 88)) return;
        CuePreview(targetSceneId);
        if (!_host.ApplyPreviewBindings(targetSceneId, Recommendation.SlotBindings, out var finalBindingReason))
        {
            _pendingSceneId = null;
            _pendingBindingKey = null;
            AutomationLastAction = $"Holding Program: Preview binding failed. {finalBindingReason}";
            return;
        }
        _pendingBindingsApplied = true;

        if (AutomationAutoTakeEnabled && Recommendation.Confidence >= 88)
        {
            AutomationLastAction = $"Taking {RecommendedSceneName} to program";
            _ = TakeAutomationPreviewAsync(targetSceneId, Recommendation.SlotBindings.ToArray());
        }
        else
        {
            AutomationLastAction = $"{RecommendedSceneName} queued on preview";
            _host.CommandStatus = AutomationLastAction;
        }
    }

    private async Task TakeAutomationPreviewAsync(string targetSceneId,
        IReadOnlyList<CoreVideoPro.MediaCore.Models.NativeDirectorSlotBinding> takenBindings)
    {
        if (_takeInFlight || !string.Equals(_host.PreviewSceneId, targetSceneId, StringComparison.Ordinal) || !_host.CanTake)
        {
            return;
        }

        _takeInFlight = true;
        var generation = _modeGeneration;
        try
        {
            await _host.TakeAsync();
            if (generation != _modeGeneration) return;
            _pendingSceneId = null;
            _pendingBindingKey = null;
            _pendingBindingsApplied = false;
            _pendingSince = null;
            if (!_host.IsMediaCoreRunning || !string.Equals(_host.ActiveSceneId, targetSceneId, StringComparison.Ordinal))
            {
                ProductionMode = ProductionMode.Manual;
                AutomationLastAction = "Automation paused � Take did not reach program";
                return;
            }
            var sceneName = _host.Scenes.FirstOrDefault(scene => scene.Id == targetSceneId)?.Name ?? targetSceneId;
            _lastTakenBindings = takenBindings;
            AutomationLastAction = $"{sceneName} taken by automation";
        }
        catch (Exception ex)
        {
            if (generation == _modeGeneration)
            {
                ProductionMode = ProductionMode.Manual;
                AutomationLastAction = $"Automation paused � Take failed: {ex.Message}";
                _host.CommandStatus = AutomationLastAction;
            }
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
        _modeGeneration++;
        if (value != ProductionMode.SetAndForget) _timer.Stop();
        _pendingSceneId = null;
        _pendingBindingKey = null;
        _pendingBindingsApplied = false;
        _pendingSince = null;
        _bindingHold.Reset();
        _lastTakenBindings = null;
        _overlayPolicyDirty = true;
        AutomationButtonLabel = value == ProductionMode.SetAndForget ? "Automation enabled" : "Automation disabled";
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
        _overlayPolicyDirty = true;
        _pendingSceneId = null;
        _pendingBindingKey = null;
        _pendingBindingsApplied = false;
        _pendingSince = null;
        _host.NotifyAutomationPolicySummaries();
        _host.RefreshProgramLowerThirdKeyPosition();
        _host.RefreshProductionReadouts();
    }
}
