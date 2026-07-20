using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels.MagicScene;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Characterization tests for <see cref="MagicSceneCoordinator"/> — the automation policy
/// engine extracted from the StudioViewModel god object (PR1 strangler). These exist because
/// the coordinator is constructible in isolation (via <see cref="FakeMagicSceneHost"/>), which
/// StudioViewModel itself is not (it new()s ~10 services + launches the native core).
/// </summary>
public sealed class MagicSceneCoordinatorTests
{
    private static readonly IReadOnlyList<Scene> Scenes =
    [
        new() { Id = "intro", Name = "Intro", Layout = "host-focus" },
        new() { Id = "interview", Name = "Interview", Layout = "two-up" },
        new() { Id = "speaker-slides", Name = "Speaker + Slides", Layout = "speaker-slides" }
    ];

    private static AutoProductionState Recommendation(string sceneId, int confidence) => new()
    {
        RecommendedSceneId = sceneId,
        Confidence = confidence,
        Reason = "test",
        Action = "hold"
    };

    private static (MagicSceneCoordinator Coordinator, FakeMagicSceneHost Host) Build()
    {
        var host = new FakeMagicSceneHost { Scenes = Scenes };
        var coordinator = new MagicSceneCoordinator(host);
        return (coordinator, host);
    }

    [Fact]
    public void ProductionModeTransition_RunsPolicyEval_StartsAndStopsTimer()
    {
        var (coordinator, host) = Build();

        coordinator.ProductionMode = ProductionMode.SetAndForget;

        Assert.True(host.Timer.IsRunning);
        Assert.True(host.Timer.StartCount >= 1);

        coordinator.ProductionMode = ProductionMode.Manual;

        Assert.False(host.Timer.IsRunning);
        Assert.Equal("Manual mode - automation is not changing scenes", coordinator.AutomationLastAction);
    }

    [Fact]
    public async Task AutoTake_FiresOnce_WhenConfidenceMeetsThresholdAndDelayElapsedAndNotInFlight()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.ActiveSceneId = "intro";
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0; // elapsed immediately
        coordinator.AutomationConfidenceThreshold = 70;
        coordinator.AutomationAutoTakeEnabled = true;
        coordinator.Recommendation = Recommendation("interview", confidence: 90);

        // Transition to SetAndForget: first eval only ARMS the pending scene (hold step).
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        Assert.Equal(0, host.TakeAsyncCallCount);

        // Second eval: pending == target, delay elapsed -> cue preview + auto-take.
        coordinator.EvaluateAutomationPolicy();
        await host.DrainTakeAsync();
        Assert.Equal(1, host.TakeAsyncCallCount);
        Assert.Equal("interview", host.PreviewSceneId);
    }

    [Fact]
    public void AutoTake_DoesNotFire_WhenConfidenceBelowThreshold()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.ActiveSceneId = "intro";
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.AutomationConfidenceThreshold = 70;
        coordinator.AutomationAutoTakeEnabled = true;
        coordinator.Recommendation = Recommendation("interview", confidence: 40);

        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();

        Assert.Equal(0, host.TakeAsyncCallCount);
        Assert.Contains("confidence 40% is below 70%", coordinator.AutomationLastAction);
    }

    [Fact]
    public async Task AutoTake_DoesNotReenter_WhileTakeIsInFlight()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.ActiveSceneId = "intro";
        host.CanTake = true;
        host.HoldTakeAsync = true; // keep the take pending
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.AutomationConfidenceThreshold = 70;
        coordinator.AutomationAutoTakeEnabled = true;
        coordinator.Recommendation = Recommendation("interview", confidence: 90);

        coordinator.ProductionMode = ProductionMode.SetAndForget; // eval #1 arms pending
        coordinator.EvaluateAutomationPolicy();                   // eval #2 starts the take (in flight)
        coordinator.EvaluateAutomationPolicy();                   // eval #3 must be blocked by the in-flight guard

        Assert.Equal(1, host.TakeAsyncCallCount);

        await host.DrainTakeAsync();
        Assert.Equal(1, host.TakeAsyncCallCount);
    }

    [Fact]
    public void ResetAutomationDefaults_RestoresDocumentedDefaults()
    {
        var (coordinator, host) = Build();
        coordinator.AutomationAutoTakeEnabled = false;
        coordinator.AutomationPreferScreenShare = false;
        coordinator.AutomationLowerThirdsEnabled = false;
        coordinator.AutomationAutoAssignInputsEnabled = false;
        coordinator.AutomationCaptionsEnabled = false;
        coordinator.AutomationConfidenceThreshold = 12;
        coordinator.AutomationSwitchDelaySeconds = 9;
        coordinator.AutomationPanelParticipantThreshold = 7;

        coordinator.ResetAutomationDefaultsCommand.Execute(null);

        Assert.True(coordinator.AutomationAutoTakeEnabled);
        Assert.True(coordinator.AutomationPreferScreenShare);
        Assert.True(coordinator.AutomationLowerThirdsEnabled);
        Assert.True(coordinator.AutomationAutoAssignInputsEnabled);
        Assert.True(coordinator.AutomationCaptionsEnabled);
        Assert.Equal(70, coordinator.AutomationConfidenceThreshold);
        Assert.Equal(4, coordinator.AutomationSwitchDelaySeconds);
        Assert.Equal(4, coordinator.AutomationPanelParticipantThreshold);
        Assert.Equal("Automation defaults restored", coordinator.AutomationLastAction);
        Assert.Equal("Automation defaults restored", host.CommandStatus);
    }

    [Fact]
    public void SetAndForgetGate_RespectsLicenseEntitlement()
    {
        var (coordinator, host) = Build();
        host.SetAndForgetEntitled = false;

        coordinator.ToggleAutomationCommand.Execute(null);
        Assert.Equal(ProductionMode.Manual, coordinator.ProductionMode);
        Assert.Equal("Set & Forget is locked on this license tier", host.CommandStatus);

        host.SetAndForgetEntitled = true;
        coordinator.ToggleAutomationCommand.Execute(null);
        Assert.Equal(ProductionMode.SetAndForget, coordinator.ProductionMode);
    }

    [Fact]
    public void RunMagicScene_RequiresActiveMeeting()
    {
        var (coordinator, host) = Build();
        host.IsInMeeting = false;
        host.PreviewSceneId = "intro";
        coordinator.Recommendation = Recommendation("interview", confidence: 90);

        coordinator.RunMagicSceneCommand.Execute(null);
        Assert.Equal("intro", host.PreviewSceneId); // unchanged
        Assert.Equal("Magic Scene requires an active Zoom meeting", host.CommandStatus);

        host.IsInMeeting = true;
        coordinator.RunMagicSceneCommand.Execute(null);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Contains("queued by Magic Scene", host.CommandStatus);
    }

    private sealed class FakeAutomationTimer : IAutomationTimer
    {
        public bool IsRunning { get; private set; }

        public int StartCount { get; private set; }

        public void Start()
        {
            IsRunning = true;
            StartCount++;
        }

        public void Stop() => IsRunning = false;
    }

    private sealed class FakeMagicSceneHost : IMagicSceneHost
    {
        private TaskCompletionSource<bool>? _pendingTake;

        public FakeAutomationTimer Timer { get; } = new();

        public int RoomVideoParticipantCount { get; set; }

        public IReadOnlyList<Scene> Scenes { get; set; } = [];

        public string ActiveSceneId { get; set; } = "intro";

        public string PreviewSceneId { get; set; } = "intro";

        public bool CanTake { get; set; }

        public bool IsInMeeting { get; set; } = true;

        public bool SetAndForgetEntitled { get; set; } = true;

        public string? CommandStatus { get; private set; }

        public string CaptionText { get; set; } = string.Empty;

        public string CaptionSpeaker { get; set; } = string.Empty;

        public int TakeAsyncCallCount { get; private set; }

        public bool HoldTakeAsync { get; set; }

        string IMagicSceneHost.CommandStatus
        {
            set => CommandStatus = value;
        }

        public Task TakeAsync()
        {
            TakeAsyncCallCount++;
            if (!HoldTakeAsync)
            {
                return Task.CompletedTask;
            }

            _pendingTake = new TaskCompletionSource<bool>();
            return _pendingTake.Task;
        }

        public async Task DrainTakeAsync()
        {
            _pendingTake?.TrySetResult(true);
            if (_pendingTake is not null)
            {
                await _pendingTake.Task;
            }
        }

        public bool IsSetAndForgetEntitled() => SetAndForgetEntitled;

        public void ApplyTransportAutomationState(ProductionMode mode, bool canToggle) { }

        public bool SetAutomationGraphic(string kind, bool enabled) => false;

        public void SchedulePreviewRoutingRefresh() { }

        public void RefreshSceneItems() { }

        public void RefreshProductionReadouts() { }

        public void RefreshProgramLowerThirdKeyPosition() { }

        public void NotifyEnabledGraphicsChanged() { }

        public void RequestMediaCoreSync() { }

        public void ReapplyShowInputAutoAssign() { }

        public void NotifyAutomationPolicySummaries() { }

        public IAutomationTimer CreateAutomationTimer(Action onTick) => Timer;
    }
}
