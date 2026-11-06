using CoreVideoPro.Control;

namespace CoreVideoPro.WinUI.ViewModels.MagicScene;

public sealed partial class MagicSceneCoordinator
{
    private ControlAutomationDiagnostic? _diagnostic;
    private long _diagnosticSequence;

    // HTTP reads this immutable publication; it never enumerates the coordinator's
    // mutable state from a transport thread or evaluates policy merely by observing it.
    public ControlAutomationDiagnostic? Diagnostic => Volatile.Read(ref _diagnostic);

    private void PublishDiagnostic()
    {
        var recommendation = Recommendation;
        static string Bounded(string? value) => value is null ? string.Empty
            : value.Length <= 512 ? value : value[..512];
        var bindings = recommendation.SlotBindings.Take(16)
            .Select(binding => new ControlAutomationBinding(binding.SlotIndex,
                Bounded(binding.PersonId), Bounded(binding.SourceId))).ToArray();
        Volatile.Write(ref _diagnostic, new ControlAutomationDiagnostic
        {
            Sequence = ++_diagnosticSequence,
            ObservedUtc = DateTimeOffset.UtcNow,
            ModeGeneration = _modeGeneration,
            Mode = ProductionMode.ToString(),
            LastAction = Bounded(AutomationLastAction),
            RecommendationSceneId = Bounded(recommendation.RecommendedSceneId),
            Confidence = recommendation.Confidence,
            RecommendationReason = Bounded(recommendation.Reason),
            Bindings = Array.AsReadOnly(bindings),
            BindingsTruncated = recommendation.SlotBindings.Count > bindings.Length,
            PendingSceneId = _pendingSceneId is null ? null : Bounded(_pendingSceneId),
            PendingElapsedMs = _pendingSince is { } since ? Math.Max(0, (_clock() - since).TotalMilliseconds) : 0,
            TakeInFlight = _takeInFlight,
            CanTake = _host.CanTake,
            PreviewSceneId = Bounded(_host.PreviewSceneId),
            ActiveSceneId = Bounded(_host.ActiveSceneId),
            CoreEpoch = Bounded(_host.CoreEpoch),
            RosterEpoch = Bounded(_host.RosterEpoch),
            AutoTake = AutomationAutoTakeEnabled,
            ConfidenceThreshold = AutomationConfidenceThreshold,
            SwitchDelaySeconds = AutomationSwitchDelaySeconds,
            PreferScreenShare = AutomationPreferScreenShare,
            PanelGuestThreshold = (int)Math.Round(AutomationPanelParticipantThreshold)
        });
    }
}
