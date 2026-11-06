using CoreVideoPro.WinUI.Models;
using CoreVideoPro.MediaCore.Models;
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
    [Fact]
    public void NativeDecisionAdvancesWithoutAnyStructuralRosterRefresh()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 8;
        host.CanTake = true;
        var latest = Recommendation("intro", 0);
        coordinator.Recommendation = latest;
        host.RefreshRecommendation = () => coordinator.Recommendation = latest;
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        latest = Recommendation("panel", 93);
        coordinator.EvaluateAutomationPolicy();
        now = now.AddSeconds(5);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("panel", host.ActiveSceneId);
        Assert.Equal(1, host.TakeAsyncCallCount);
        Assert.Equal(93, coordinator.Diagnostic!.Confidence);
    }

    [Fact]
    public void MagicSceneReadsLatestDecisionWithoutStructuralRosterRefresh()
    {
        var (coordinator, host) = Build();
        coordinator.Recommendation = Recommendation("intro", 0);
        host.RefreshRecommendation = () => coordinator.Recommendation = Recommendation("interview", 93);
        coordinator.RunMagicSceneCommand.Execute(null);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(2, host.PreviewBindings.Count);
        Assert.Equal(0, host.TakeAsyncCallCount);
    }

    [Fact]
    public void DiagnosticPreservesTheObservedHoldAndDoesNotActuateWhenRead()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 3;
        host.CanTake = false;
        coordinator.Recommendation = Recommendation("interview", 93);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddSeconds(5);
        coordinator.EvaluateAutomationPolicy();
        var held = Assert.IsType<CoreVideoPro.Control.ControlAutomationDiagnostic>(coordinator.Diagnostic);
        Assert.Contains("Take is not ready", held.LastAction);
        Assert.False(held.CanTake);
        Assert.Equal("interview", held.PreviewSceneId);
        Assert.Equal("intro", held.ActiveSceneId);
        Assert.Equal(93, held.Confidence);
        Assert.Equal(5000, held.PendingElapsedMs);
        Assert.Equal(2, held.Bindings.Count);
        Assert.Same(held, coordinator.Diagnostic);
        Assert.Equal(0, host.TakeAsyncCallCount);

        host.CanTake = true;
        coordinator.EvaluateAutomationPolicy();
        var taken = coordinator.Diagnostic!;
        Assert.True(taken.Sequence > held.Sequence);
        Assert.Equal("interview", taken.ActiveSceneId);
        Assert.Contains("taken by automation", taken.LastAction);
        Assert.False(taken.TakeInFlight);
        Assert.Equal(1, host.TakeAsyncCallCount);
        Assert.Equal("intro", held.ActiveSceneId);
        Assert.False(held.CanTake);
    }

    private static readonly IReadOnlyList<Scene> Scenes =
    [
        new() { Id = "intro", Name = "Intro", Layout = "host-focus" },
        new() { Id = "interview", Name = "Interview", Layout = "two-up" },
        new() { Id = "panel", Name = "Panel", Layout = "grid" },
        new() { Id = "speaker-slides", Name = "Speaker + Slides", Layout = "speaker-slides" }
    ];

    private static AutoProductionState Recommendation(string sceneId, int confidence) => new()
    {
        RecommendedSceneId = sceneId,
        Confidence = confidence,
        Reason = "test",
        Action = "hold",
        SlotBindings = sceneId == "interview"
            ? [new NativeDirectorSlotBinding { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" },
               new NativeDirectorSlotBinding { SlotIndex = 1, PersonId = "b", SourceId = "zoom:b" }]
            : [new NativeDirectorSlotBinding { SlotIndex = 0, PersonId = "a", SourceId = "zoom:a" }]
    };

    private static (MagicSceneCoordinator Coordinator, FakeMagicSceneHost Host) Build(Func<DateTimeOffset>? clock = null)
    {
        var host = new FakeMagicSceneHost { Scenes = Scenes };
        var coordinator = new MagicSceneCoordinator(host, clock);
        return (coordinator, host);
    }

    [Fact]
    public void SerializedNativeShotWithCurrentPolicyCuesAndTakesTheIntendedGuests()
    {
        const string json = """{"recommendedSceneId":"interview","confidence":92,"rationale":"Two guests have the floor","policy":{"preferScreenShare":false,"panelGuestThreshold":4},"slotBindings":[{"slotIndex":0,"personId":"a","sourceId":"zoom:a"},{"slotIndex":1,"personId":"b","sourceId":"zoom:b"}]}""";
        var native = System.Text.Json.JsonSerializer.Deserialize<NativeMediaCoreAutoProduction>(json,
            new System.Text.Json.JsonSerializerOptions(System.Text.Json.JsonSerializerDefaults.Web));
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 4;
        host.CanTake = true;
        coordinator.AutomationPreferScreenShare = false;
        coordinator.Recommendation = ProductionStateHelper.BuildAutomationRecommendation(native,
            Enumerable.Range(0, 4).Select(i => new Participant { Id = i.ToString() }).ToArray(), Scenes, false, 4);
        coordinator.ToggleAutomationCommand.Execute(null);
        Assert.Equal("intro", host.ActiveSceneId);
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(new[] { "a", "b" }, host.PreviewBindings.Select(binding => binding.PersonId));
        now = now.AddSeconds(4);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);
        Assert.Equal("interview", host.ActiveSceneId);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);
    }

    [Theory]
    [InlineData("confidence")]
    [InlineData("bindings")]
    [InlineData("core")]
    [InlineData("meeting")]
    public void InterruptedPersonHoldMustEarnItsFullStabilityAgain(string interruption)
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("interview", 92);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(1000);
        if (interruption == "confidence") coordinator.Recommendation = Recommendation("interview", 0);
        if (interruption == "bindings") coordinator.Recommendation = new AutoProductionState
            { RecommendedSceneId = "interview", Confidence = 92, Reason = "no bindings", Action = "hold", SlotBindings = [] };
        if (interruption == "core") host.IsMediaCoreRunning = false;
        if (interruption == "meeting") host.IsInMeeting = false;
        coordinator.EvaluateAutomationPolicy();
        now = now.AddSeconds(20);
        host.IsMediaCoreRunning = true;
        host.IsInMeeting = true;
        coordinator.Recommendation = Recommendation("interview", 92);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(0, host.TakeAsyncCallCount);
        Assert.Equal("interview", host.PreviewSceneId); // warm Preview while Program holds
        Assert.Equal("intro", host.ActiveSceneId);
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);
    }

    [Fact]
    public void PolicyChangesRequestNativeSyncAndExplainTakeThreshold()
    {
        var (coordinator, host) = Build();
        coordinator.AutomationPreferScreenShare = false;
        Assert.Equal(1, host.SyncRequests);
        Assert.False(coordinator.DirectorPolicy.PreferScreenShare);
        coordinator.AutomationPanelParticipantThreshold = 7;
        Assert.Equal(2, host.SyncRequests);
        Assert.Equal(7, coordinator.DirectorPolicy.PanelGuestThreshold);
        Assert.Contains("Take at 88%", coordinator.TimingSummary);
        coordinator.AutomationConfidenceThreshold = 95;
        Assert.Contains("Take at 95%", coordinator.TimingSummary);
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
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
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

        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy(); // binding hold completes and arms scene hold
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
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.ActiveSceneId = "intro";
        host.CanTake = true;
        host.HoldTakeAsync = true; // keep the take pending
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.AutomationConfidenceThreshold = 70;
        coordinator.AutomationAutoTakeEnabled = true;
        coordinator.Recommendation = Recommendation("interview", confidence: 90);

        coordinator.ProductionMode = ProductionMode.SetAndForget; // eval #1 arms pending
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();                   // binding hold completes
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
        Assert.False(coordinator.AutomationCaptionsEnabled);
        Assert.Equal(70, coordinator.AutomationConfidenceThreshold);
        Assert.Equal(4, coordinator.AutomationSwitchDelaySeconds);
        Assert.Equal(4, coordinator.AutomationPanelParticipantThreshold);
        Assert.Equal("Automation defaults restored", coordinator.AutomationLastAction);
        Assert.Equal("Automation defaults restored", host.CommandStatus);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void AutoMode_NeverEnablesCaptionStripOrClearsExternalCaptionText(bool legacyEnable)
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.CaptionText = "Operator supplied cue";
        host.CaptionSpeaker = "Presenter";
        coordinator.AutomationCaptionsEnabled = legacyEnable;
        coordinator.ToggleAutomationCommand.Execute(null);
        coordinator.EvaluateAutomationPolicy();
        coordinator.ResetAutomationDefaultsCommand.Execute(null);
        coordinator.EvaluateAutomationPolicy();

        Assert.False(coordinator.AutomationCaptionsEnabled);
        Assert.Contains(("caption", false), host.GraphicPolicies);
        Assert.DoesNotContain(("caption", true), host.GraphicPolicies);
        Assert.Contains(("lower-third", true), host.GraphicPolicies);
        Assert.Equal("Operator supplied cue", host.CaptionText);
        Assert.Equal("Presenter", host.CaptionSpeaker);
    }

    [Fact]
    public void LegacyCaptionEnable_IsRejectedWithoutAdvertisingAnEnabledFeature()
    {
        var (coordinator, host) = Build();
        coordinator.AutomationCaptionsEnabled = true;
        Assert.False(coordinator.AutomationCaptionsEnabled);
        Assert.Contains("does not transcribe speech", host.CommandStatus);
        Assert.Empty(host.GraphicPolicies);
    }

    // #767: the licence tier was never real (a mock panel with nothing behind it), so
    // Set & Forget has no entitlement gate; the toggle is the operator's alone.
    [Fact]
    public void SetAndForget_TogglesWithoutALicenseGate()
    {
        var (coordinator, host) = Build();

        coordinator.ToggleAutomationCommand.Execute(null);
        Assert.Equal(ProductionMode.SetAndForget, coordinator.ProductionMode);
        Assert.StartsWith("Set & Forget enabled", host.CommandStatus);

        coordinator.ToggleAutomationCommand.Execute(null);
        Assert.Equal(ProductionMode.Manual, coordinator.ProductionMode);
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
        Assert.Equal(["a", "b"], host.PreviewBindings.Select(binding => binding.PersonId).ToArray());
    }

    [Fact]
    public void MagicSceneWithNoBindingsExplainsWhyPreviewCannotBePopulated()
    {
        var (coordinator, host) = Build();
        coordinator.Recommendation = new AutoProductionState
        {
            RecommendedSceneId = "intro", Confidence = 90,
            Reason = "No guest video or host camera is available.", Action = "hold"
        };
        coordinator.RunMagicSceneCommand.Execute(null);
        Assert.Contains("no source to bind on Preview", coordinator.MagicSceneStatus);
        Assert.Contains("No guest video", host.CommandStatus);
        Assert.Empty(host.PreviewBindings);
        Assert.Equal("intro", host.ActiveSceneId);
    }

    [Fact]
    public void MagicSceneOnlyCuesPreviewWithoutChangingLiveGraphics()
    {
        var (coordinator, host) = Build();
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.RunMagicSceneCommand.Execute(null);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal(["a", "b"], host.PreviewBindings.Select(binding => binding.PersonId).ToArray());
        Assert.Equal(0, host.GraphicPolicyCalls);
        Assert.Equal(0, host.TakeAsyncCallCount);
    }

    [Fact]
    public void SetAndForgetQueuesPreviewBelowEightyEightWithoutTaking()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("interview", 80);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal(["a", "b"], host.PreviewBindings.Select(binding => binding.PersonId).ToArray());
        Assert.Equal(0, host.TakeAsyncCallCount);
    }

    [Fact]
    public void SetAndForgetKeepsTwoUpForEightSecondsButMagicSceneCuesImmediately()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.ActiveSceneId = "interview";
        host.PreviewSceneId = "interview";
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("intro", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(7999);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal("interview", host.ActiveSceneId);
        Assert.Equal(0, host.TakeAsyncCallCount);
        now = now.AddMilliseconds(1);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal("a", Assert.Single(host.PreviewBindings).PersonId);

        var (oneShot, manualHost) = Build();
        manualHost.ActiveSceneId = "interview";
        manualHost.PreviewSceneId = "interview";
        oneShot.Recommendation = Recommendation("intro", 90);
        oneShot.RunMagicSceneCommand.Execute(null);
        Assert.Equal("intro", manualHost.PreviewSceneId);
        Assert.Equal(0, manualHost.TakeAsyncCallCount);
    }

    [Fact]
    public void SetAndForgetHoldsFollowSpeakerBindingOnTheSameScene()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("intro", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy(); // establish the first Program binding
        Assert.Equal(1, host.TakeAsyncCallCount);

        coordinator.Recommendation = new AutoProductionState
        {
            RecommendedSceneId = "intro", Confidence = 90, Reason = "new guest speaker", Action = "hold",
            SlotBindings = [new NativeDirectorSlotBinding { SlotIndex = 0, PersonId = "b", SourceId = "zoom:b" }]
        };
        coordinator.EvaluateAutomationPolicy();
        now = now.AddMilliseconds(1499);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);
        now = now.AddMilliseconds(1);
        coordinator.EvaluateAutomationPolicy(); // arm scene hold
        coordinator.EvaluateAutomationPolicy(); // take bound Preview
        Assert.Equal(2, host.TakeAsyncCallCount);
        Assert.Equal("b", Assert.Single(host.PreviewBindings).PersonId);
    }

    [Fact]
    public void ManualPreviewSelectionDisablesAutomationAndLeavesSelectionAlone()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        host.PreviewSceneId = "speaker-slides";
        coordinator.NotifyPreviewSceneChanged();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(ProductionMode.Manual, coordinator.ProductionMode);
        Assert.Equal("Automation disabled", coordinator.AutomationButtonLabel);
        Assert.False(host.Timer.IsRunning);
        Assert.Equal("speaker-slides", host.PreviewSceneId);
    }

    [Fact]
    public void SnapshotRefreshDoesNotContinuouslyReassertGraphicPolicy()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        coordinator.Recommendation = Recommendation("intro", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        var calls = host.GraphicPolicyCalls;
        Assert.Equal(2, calls);
        for (var i = 0; i < 100; i++) coordinator.EvaluateAutomationPolicy();
        Assert.Equal(calls, host.GraphicPolicyCalls);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void FailedAutoTakePausesInsteadOfRetryingOrClaimingSuccess(bool throws)
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        host.FailTake = true;
        host.ThrowTake = throws;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();
        for (var i = 0; i < 10; i++) coordinator.EvaluateAutomationPolicy();
        Assert.Equal(ProductionMode.Manual, coordinator.ProductionMode);
        Assert.Equal(1, host.TakeAsyncCallCount);
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.DoesNotContain("taken by automation", coordinator.AutomationLastAction);
    }

    [Fact]
    public async Task PendingTakeFreezesPreviewAndDisabledModeIgnoresLateCompletion()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        host.HoldTakeAsync = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        coordinator.Recommendation = Recommendation("speaker-slides", 90);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("interview", host.PreviewSceneId);
        coordinator.NotifyManualSceneSelection();
        host.PreviewSceneId = "speaker-slides";
        await host.DrainTakeAsync();
        Assert.Equal("Manual mode - automation is not changing scenes", coordinator.AutomationLastAction);
        Assert.Equal(1, host.TakeAsyncCallCount);
        Assert.Equal("Manual", coordinator.Diagnostic!.Mode);
        Assert.False(coordinator.Diagnostic.TakeInFlight);
        Assert.Equal(coordinator.AutomationLastAction, coordinator.Diagnostic.LastAction);
    }

    [Fact]
    public void RejectedSameSceneTakePausesAndCanBeRetriedAfterOperatorResumes()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        host.FailTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("intro", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal(ProductionMode.Manual, coordinator.ProductionMode);
        Assert.Contains("test take rejected", coordinator.AutomationLastAction);
        Assert.Equal(coordinator.AutomationLastAction, host.CommandStatus);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);

        host.FailTake = false;
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(2, host.TakeAsyncCallCount);
        Assert.Contains("taken by automation", coordinator.AutomationLastAction);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(2, host.TakeAsyncCallCount);
    }

    [Theory]
    [InlineData("core")]
    [InlineData("meeting")]
    [InlineData("offline")]
    [InlineData("leave")]
    public void SessionChangeRebindsEvenWhenSceneAndPeopleIdsAreReused(string change)
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("intro", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);

        if (change == "core") host.CoreEpoch = "core-2";
        if (change == "meeting") host.RosterEpoch = "meeting-2";
        if (change == "offline") host.IsMediaCoreRunning = false;
        if (change == "leave") host.IsInMeeting = false;
        coordinator.EvaluateAutomationPolicy();
        host.IsMediaCoreRunning = true;
        host.IsInMeeting = true;
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(2, host.TakeAsyncCallCount);
        Assert.Equal(ProductionMode.SetAndForget, coordinator.ProductionMode);
    }

    [Theory]
    [InlineData("intro", "interview", 4000)]
    [InlineData("interview", "intro", 8000)]
    [InlineData("intro", "intro", 4000)]
    [InlineData("panel", "panel", 6000)]
    [InlineData("intro", "speaker-slides", 2500)]
    [InlineData("speaker-slides", "intro", 3500)]
    public void PersonAndSceneHoldsShareOneDeadlineWhilePreviewWarms(string from, string to, int totalMs)
    {
        static AutoProductionState Shot(string scene, params string[] people) => new()
        {
            RecommendedSceneId = scene, Confidence = 95, Reason = "scripted conversation", Action = "hold",
            SlotBindings = people.Select((id, slot) => new NativeDirectorSlotBinding
                { SlotIndex = slot, PersonId = id, SourceId = "zoom:" + id }).ToArray()
        };
        var now = DateTimeOffset.UnixEpoch;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 4;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Shot(from, from == "panel" ? ["a", "b", "c"]
            : from == "interview" ? ["a", "b"] : ["a"]);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddSeconds(10);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);

        coordinator.AutomationSwitchDelaySeconds = 4;
        coordinator.Recommendation = Shot(to, to is "panel" or "interview" ? ["a", "b"] : ["b"]);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(to, host.PreviewSceneId);
        Assert.Equal(coordinator.Recommendation.SlotBindings, host.PreviewBindings);
        Assert.Equal(from, host.ActiveSceneId);
        now = now.AddMilliseconds(totalMs - 1);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);
        now = now.AddMilliseconds(1);
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(2, host.TakeAsyncCallCount);
        Assert.Equal(to, host.ActiveSceneId);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task PriorSessionTakeCompletionCannotClaimSuccessOrPauseNewSession(bool observeBeforeCompletion, bool failed)
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        host.HoldTakeAsync = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("intro", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(1, host.TakeAsyncCallCount);

        host.CoreEpoch = "core-2";
        if (observeBeforeCompletion) coordinator.EvaluateAutomationPolicy();
        host.FailTake = failed;
        await host.DrainTakeAsync();
        Assert.Equal(ProductionMode.SetAndForget, coordinator.ProductionMode);
        Assert.DoesNotContain("taken by automation", coordinator.AutomationLastAction);
        host.HoldTakeAsync = false;
        host.FailTake = false;
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(2, host.TakeAsyncCallCount);
        Assert.Contains("taken by automation", coordinator.AutomationLastAction);
    }

    [Fact]
    public void AutomationCueSurvivesPreviewCallbackAndNestedReadoutRefresh()
    {
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        host.OnPreviewChanged = () =>
        {
            coordinator.NotifyPreviewSceneChanged();
            coordinator.EvaluateAutomationPolicy();
        };
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(ProductionMode.SetAndForget, coordinator.ProductionMode);
        Assert.Equal("interview", host.ActiveSceneId);
        Assert.Equal(1, host.TakeAsyncCallCount);
    }

    [Fact]
    public void OfflineCoreHoldsAutomationButAllowsExplicitMagicPreviewCue()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        host.IsMediaCoreRunning = false;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("intro", host.PreviewSceneId);
        Assert.Equal(0, host.TakeAsyncCallCount);
        coordinator.RunMagicSceneCommand.Execute(null);
        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal(0, host.TakeAsyncCallCount);
    }

    [Fact]
    public void MissingSceneOrDisconnectedMeetingCannotCueOrTake()
    {
        var (coordinator, host) = Build();
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 0;
        coordinator.Recommendation = Recommendation("deleted", 90);
        coordinator.RunMagicSceneCommand.Execute(null);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal("intro", host.PreviewSceneId);
        host.IsInMeeting = false;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.EvaluateAutomationPolicy();
        coordinator.EvaluateAutomationPolicy();
        Assert.Equal(0, host.TakeAsyncCallCount);
        Assert.Equal("intro", host.PreviewSceneId);
    }

    [Fact]
    public void AutomationCuesPreviewAtTheStartOfTheHoldSoTheTargetWarmsBeforeTheTake()
    {
        // #478 R5: only sources are subscribed, so a scene's guests have no feed until the
        // scene is on a bus. Cueing at the Take (the old behaviour) cut Program to a cold
        // subscription; cueing when the hold STARTS gives the whole hold to warm it.
        var now = DateTimeOffset.UtcNow;
        var (coordinator, host) = Build(() => now);
        host.RoomVideoParticipantCount = 2;
        host.CanTake = true;
        coordinator.AutomationSwitchDelaySeconds = 30;
        coordinator.Recommendation = Recommendation("interview", 90);
        coordinator.ProductionMode = ProductionMode.SetAndForget;
        now = now.AddMilliseconds(1200);
        coordinator.EvaluateAutomationPolicy();

        Assert.Equal("interview", host.PreviewSceneId);
        Assert.Equal("intro", host.ActiveSceneId);
        Assert.Equal(0, host.TakeAsyncCallCount);
        Assert.Equal(ProductionMode.SetAndForget, coordinator.ProductionMode);
        Assert.StartsWith("Holding", coordinator.AutomationLastAction);
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

        private string _previewSceneId = "intro";
        public Action? OnPreviewChanged { get; set; }
        public string PreviewSceneId
        {
            get => _previewSceneId;
            set { _previewSceneId = value; OnPreviewChanged?.Invoke(); }
        }

        public bool CanTake { get; set; }

        public bool IsInMeeting { get; set; } = true;

        public bool IsMediaCoreRunning { get; set; } = true;

        public string CoreEpoch { get; set; } = "core-1";
        public string RosterEpoch { get; set; } = "meeting-1";

        public string? CommandStatus { get; private set; }

        public string CaptionText { get; set; } = string.Empty;

        public string CaptionSpeaker { get; set; } = string.Empty;

        public int TakeAsyncCallCount { get; private set; }

        public bool HoldTakeAsync { get; set; }
        public bool FailTake { get; set; }
        public bool ThrowTake { get; set; }
        public int GraphicPolicyCalls { get; private set; }
        public List<(string Kind, bool Enabled)> GraphicPolicies { get; } = [];
        public IReadOnlyList<NativeDirectorSlotBinding> PreviewBindings { get; private set; } = [];

        string IMagicSceneHost.CommandStatus
        {
            set => CommandStatus = value;
        }

        public async Task<CoreVideoPro.WinUI.ViewModels.Transport.TakeResult> TakeAsync()
        {
            TakeAsyncCallCount++;
            var target = PreviewSceneId;
            if (ThrowTake) throw new InvalidOperationException("test take failed");
            if (HoldTakeAsync)
            {
                _pendingTake = new TaskCompletionSource<bool>();
                await _pendingTake.Task;
            }
            if (!FailTake) ActiveSceneId = target;
            return FailTake ? CoreVideoPro.WinUI.ViewModels.Transport.TakeResult.Failed("test take rejected")
                : CoreVideoPro.WinUI.ViewModels.Transport.TakeResult.Success;
        }

        public async Task DrainTakeAsync()
        {
            _pendingTake?.TrySetResult(true);
            if (_pendingTake is not null)
            {
                await _pendingTake.Task;
            }
        }

        public void ApplyTransportAutomationState(ProductionMode mode) { }

        public bool SetAutomationGraphic(string kind, bool enabled)
        {
            GraphicPolicyCalls++;
            GraphicPolicies.Add((kind, enabled));
            return false;
        }

        public void SchedulePreviewRoutingRefresh() { }

        public bool ApplyPreviewBindings(string sceneId, IReadOnlyList<NativeDirectorSlotBinding> bindings, out string reason)
        {
            if (sceneId != PreviewSceneId || bindings.Count == 0)
            {
                reason = "No guest video or host camera has a content frame.";
                return false;
            }
            PreviewBindings = bindings;
            reason = string.Empty;
            return true;
        }

        public void RefreshSceneItems() { }

        public void RefreshProductionReadouts() { }
        public Action? RefreshRecommendation { get; set; }
        public void RefreshAutomationRecommendation() => RefreshRecommendation?.Invoke();

        public void RefreshProgramLowerThirdKeyPosition() { }

        public void NotifyEnabledGraphicsChanged() { }

        public int SyncRequests { get; private set; }
        public void RequestMediaCoreSync() => SyncRequests++;

        public void ReapplyShowInputAutoAssign() { }

        public void NotifyAutomationPolicySummaries() { }

        public IAutomationTimer CreateAutomationTimer(Action onTick) => Timer;
    }
}
