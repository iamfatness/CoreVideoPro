using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Models;

/// <summary>Accept a complete native shot, never synthesize people from roster counts.</summary>
public static class NativeDirectorRecommendationPolicy
{
    public static AutoProductionState Resolve(NativeMediaCoreAutoProduction? native,
        IReadOnlyList<Scene> scenes, bool preferScreenShare, int panelGuestThreshold)
    {
        AutoProductionState Hold(string reason) => new()
        {
            RecommendedSceneId = scenes.FirstOrDefault()?.Id ?? string.Empty,
            Confidence = 0, Action = "hold", Reason = reason, SlotBindings = []
        };
        if (native is null) return Hold("Holding Program: waiting for the native director.");
        if (native.Policy is null)
            return Hold("Holding Program: the native director does not report operator policy; update the media core.");
        if (native.Policy.PreferScreenShare != preferScreenShare ||
            native.Policy.PanelGuestThreshold != Math.Clamp(panelGuestThreshold, 2, 10))
            return Hold("Holding Program: waiting for the native director to apply your settings.");
        if (!scenes.Any(scene => scene.Id == native.RecommendedSceneId))
            return Hold($"Holding Program: recommended scene '{native.RecommendedSceneId}' is unavailable.");
        if (native.Confidence <= 0 || native.SlotBindings.Count == 0)
            return Hold($"Holding Program: {native.Rationale}");
        var bindings = native.SlotBindings;
        if (bindings.Any(binding => binding.SlotIndex < 0 || string.IsNullOrWhiteSpace(binding.PersonId) ||
                string.IsNullOrWhiteSpace(binding.SourceId)) ||
            bindings.Select(binding => binding.SlotIndex).Distinct().Count() != bindings.Count ||
            bindings.Select(binding => binding.PersonId).Distinct(StringComparer.Ordinal).Count() != bindings.Count ||
            bindings.Select(binding => binding.SourceId).Distinct(StringComparer.Ordinal).Count() != bindings.Count ||
            !bindings.Select(binding => binding.SlotIndex).Order().SequenceEqual(Enumerable.Range(0, bindings.Count)) ||
            (native.RecommendedSceneId == "interview" && bindings.Count != 2))
            return Hold("Holding Program: native director returned incomplete or duplicate person bindings.");
        return new AutoProductionState
        {
            RecommendedSceneId = native.RecommendedSceneId,
            Confidence = Math.Clamp(native.Confidence, 0, 100),
            Reason = native.Rationale,
            Action = native.RuleId == "screen-share-priority" ? "hold" : "suggest",
            SlotBindings = bindings.ToArray()
        };
    }
}
