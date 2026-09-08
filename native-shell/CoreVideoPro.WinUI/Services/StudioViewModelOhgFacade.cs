using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The real <see cref="IOhgHostFacade"/> over the operator <see cref="StudioViewModel"/>
/// (Plan 7a Task 11, spec §8). Pure glue: every member routes through the SAME entry point the
/// control surface uses for the operator-equivalent action, so the slot-write scopes, the
/// CommandStatus lines, and the S2b preview-draft rules all apply unchanged.
///
/// **UI thread only.** <c>OhgHostAdapter</c> is invoked from the control surface's dispatcher
/// enqueue; nothing here marshals for itself, and nothing here blocks on a task.
///
/// The exact ViewModel members it binds to (this list IS the coupling — keep it small):
/// <list type="bullet">
/// <item><c>ShowInputEditors[slot-1]</c> (<c>Kind</c>/<c>ParticipantId</c>/<c>InShow</c>/<c>DisplayName</c>),
/// written inside <c>ShowInputWriteScope.Enter("ohg.…")</c> so the slot-write log names OHG as
/// the writer (CLAUDE.md: an untracked slot-write in launch.log is a bug);</item>
/// <item><c>Scenes</c> + <c>SelectSceneCommand</c> for cueing preview;</item>
/// <item><c>SetPreviewRouteSlots</c> (the one method Task 11 added) for the named look routes;</item>
/// <item><c>TakeCommand.CanExecute</c> + <c>SetTakeTransitionCommand</c> + <c>TakeForControlAsync</c>;</item>
/// <item><c>CaptionText</c> and <c>CommandStatus</c>.</item>
/// </list>
/// </summary>
public sealed class StudioViewModelOhgFacade : IOhgHostFacade
{
    /// <summary>Controller ruling (2026-09-07): the shell has no per-input lower-third TITLE
    /// field, so the nameplate's `location` line is dropped in 7a and carried to Plan 7b. Logged
    /// ONCE per facade lifetime — a nameplate change per look would otherwise flood the log.</summary>
    internal const string LowerThirdTitleUnsupported =
        "ohg: per-input lower-third titles are not supported by the shell yet (nameplate location dropped)";

    private static readonly HashSet<string> KnownTransitions =
        new(StringComparer.OrdinalIgnoreCase) { "cut", "fade", "dip", "wipe" };

    private readonly StudioViewModel _vm;
    private readonly Action<string> _log;

    private bool _reportedLowerThirdTitleUnsupported;

    public StudioViewModelOhgFacade(StudioViewModel viewModel, Action<string>? log = null)
    {
        _vm = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        _log = log ?? (_ => { });
    }

    public int ShowInputCount => _vm.ShowInputEditors.Count;

    public bool AssignZoomParticipant(int slot, string? participantId)
    {
        if (!TryGetEditor(slot, out var editor))
        {
            return false;
        }

        using var scope = ShowInputWriteScope.Enter("ohg.assignSlot");

        if (string.IsNullOrEmpty(participantId))
        {
            // Clear: take it out of show FIRST so no frame is composited against a half-cleared
            // slot, then drop the participant.
            editor.InShow = false;
            editor.ParticipantId = null;
            return true;
        }

        editor.Kind = ShowInputKind.ZoomParticipant;
        editor.ParticipantId = participantId;
        editor.InShow = true;
        return true;
    }

    public bool SetInputDisplayName(int slot, string name)
    {
        if (!TryGetEditor(slot, out var editor) || !editor.IsAssigned)
        {
            return false;
        }

        using var scope = ShowInputWriteScope.Enter("ohg.setNameplates");
        editor.DisplayName = name ?? string.Empty;  // blank resets to the derived name
        return true;
    }

    public bool SetInputLowerThirdTitle(int slot, string title)
    {
        // A no-op that reports SUCCESS on purpose: refusing would make every setNameplates
        // command a refusal in the engine's eyes and drown the real "slot unassigned" refusals.
        // The gap is stated once, loudly, instead.
        if (!_reportedLowerThirdTitleUnsupported)
        {
            _reportedLowerThirdTitleUnsupported = true;
            _log(LowerThirdTitleUnsupported);
        }

        return true;
    }

    public bool SceneExists(string sceneId)
        => !string.IsNullOrEmpty(sceneId) &&
           _vm.Scenes.Any(scene => string.Equals(scene.Id, sceneId, StringComparison.Ordinal));

    public IReadOnlyList<string> CueSceneWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routeSlots)
    {
        // Cue first: SetPreviewRouteSlots edits the routes of whatever scene is CURRENTLY on
        // preview, so cueing afterwards would write the look into the wrong scene.
        _vm.SelectSceneCommand.Execute(sceneId);
        return _vm.SetPreviewRouteSlots(routeSlots ?? new Dictionary<string, int?>());
    }

    public bool CanTake => _vm.TakeCommand.CanExecute(null);

    public async Task<bool> TakeAsync(string transition)
    {
        if (!IsKnownTransition(transition))
        {
            return false;
        }

        _vm.SetTakeTransitionCommand.Execute(transition);
        var result = await _vm.TakeForControlAsync().ConfigureAwait(true);
        if (!result.Succeeded && result.Error is { Length: > 0 } error)
        {
            _log($"ohg: take failed — {error}");
        }

        return result.Succeeded;
    }

    public bool IsKnownTransition(string transition)
        => !string.IsNullOrWhiteSpace(transition) && KnownTransitions.Contains(transition.Trim());

    public void SetCaption(string text) => _vm.CaptionText = text ?? string.Empty;

    public void ReportStatus(string line) => _vm.CommandStatus = line;

    private bool TryGetEditor(int slot, out ShowInputSlotViewModel editor)
    {
        // Slots are 1-based, on the engine wire exactly as on the control API (spec D10).
        var index = slot - 1;
        if (index >= 0 && index < _vm.ShowInputEditors.Count)
        {
            editor = _vm.ShowInputEditors[index];
            return true;
        }

        editor = null!;
        return false;
    }
}
