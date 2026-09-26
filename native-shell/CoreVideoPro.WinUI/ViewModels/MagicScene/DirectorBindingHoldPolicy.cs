using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.ViewModels.MagicScene;

/// <summary>Time-gates Set & Forget person-binding changes without touching routes.</summary>
public sealed class DirectorBindingHoldPolicy
{
    public static double RequiredSceneHoldSeconds(string programSceneId, string proposedSceneId,
        double ordinarySceneHoldSeconds) => proposedSceneId == "speaker-slides" ? 2.5
            : programSceneId == "speaker-slides" ? 3.5
            : ordinarySceneHoldSeconds;

    private string? _candidate;
    private DateTimeOffset _since;

    public void Reset() => _candidate = null;

    public (bool Ready, int RequiredMs, int ElapsedMs) Evaluate(
        string programSceneId,
        IReadOnlyList<NativeDirectorSlotBinding>? programBindings,
        string proposedSceneId,
        IReadOnlyList<NativeDirectorSlotBinding> proposedBindings,
        DateTimeOffset now)
    {
        var key = programSceneId + ">" + proposedSceneId + ":" + string.Join("|", proposedBindings
            .OrderBy(binding => binding.SlotIndex)
            .Select(binding => $"{binding.SlotIndex}:{binding.PersonId}:{binding.SourceId}"));
        if (_candidate != key)
        {
            _candidate = key;
            _since = now;
        }
        var required = RequiredHoldMs(programSceneId, programBindings, proposedSceneId, proposedBindings);
        var elapsed = (int)Math.Max(0, (now - _since).TotalMilliseconds);
        return (elapsed >= required, required, elapsed);
    }

    public static int RequiredHoldMs(
        string programSceneId,
        IReadOnlyList<NativeDirectorSlotBinding>? programBindings,
        string proposedSceneId,
        IReadOnlyList<NativeDirectorSlotBinding> proposedBindings)
    {
        if (programSceneId == "interview" && proposedSceneId == "intro") return 8000;
        if (programSceneId == "intro" && proposedSceneId == "interview") return 1200;
        if (programSceneId == "panel" && proposedSceneId == "panel" && programBindings is not null)
        {
            var prior = programBindings.Select(binding => binding.PersonId).ToHashSet(StringComparer.Ordinal);
            var next = proposedBindings.Select(binding => binding.PersonId).ToHashSet(StringComparer.Ordinal);
            if (prior.Except(next).Any()) return 6000;
            if (next.Except(prior).Any()) return 1000;
        }
        if (programSceneId == proposedSceneId && programBindings is not null &&
            !programBindings.Select(binding => binding.PersonId).SequenceEqual(
                proposedBindings.Select(binding => binding.PersonId))) return 1500;
        return 0;
    }
}
