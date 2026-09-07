namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The narrow surface <see cref="OhgHostAdapter"/> needs from the operator shell — the show
/// engine's host commands (spec §8) expressed as the handful of things they actually do to a
/// show, and nothing else.
///
/// It exists so the mapping from engine command to shell action is testable WITHOUT a
/// <c>StudioViewModel</c> (which is not constructible in tests: field-init
/// <c>DispatcherQueue.GetForCurrentThread()</c>, a ctor that hard-<c>new</c>s ~10 services and
/// launches the core). The real implementation over the ViewModel is
/// <c>StudioViewModelOhgFacade</c> (Task 11); it routes through the SAME entry points the
/// control surface uses, so the operator-equivalent write scopes and logs apply.
///
/// **Every member returns rather than throws.** A <c>false</c> is the shell correctly saying no,
/// which the adapter turns into a refusal string for the engine and the launch log — an
/// exception here would be a bug in the facade, not a refusal.
/// </summary>
public interface IOhgHostFacade
{
    /// <summary>How many Show Input slots the shell has. Fixed at 10 (the engine's capacity is
    /// validated equal to it before spawn).</summary>
    int ShowInputCount { get; }

    /// <summary>Point Show Input <paramref name="slot"/> at Zoom participant
    /// <paramref name="participantId"/> and put it in show; <c>null</c> clears the slot and takes
    /// it out of show. Returns false when the slot is outside 1..<see cref="ShowInputCount"/>.</summary>
    bool AssignZoomParticipant(int slot, string? participantId);

    /// <summary>Set the Show Input's operator-visible display name. False when the slot is not
    /// assigned.</summary>
    bool SetInputDisplayName(int slot, string name);

    /// <summary>Set the Show Input's lower-third title line. False when the slot is not
    /// assigned.</summary>
    bool SetInputLowerThirdTitle(int slot, string title);

    /// <summary>Does a scene with this id exist right now?</summary>
    bool SceneExists(string sceneId);

    /// <summary>Cue <paramref name="sceneId"/> to PREVIEW and, in that scene, point each named
    /// route at the given Show Input slot (<c>null</c> ⇒ the route carries no source).
    /// A route id ABSENT from <paramref name="routeSlots"/> is left exactly as it was — so an
    /// EMPTY dictionary cues the scene and rewrites nothing — while a present key with a
    /// <c>null</c> value explicitly empties that route.
    /// Returns the ids of routes that were REQUESTED but not found in the scene — the caller
    /// reports those; the rest are applied (a partly-wired look is better than none, and a
    /// silent partial application is what this return value exists to prevent).</summary>
    IReadOnlyList<string> CueSceneWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routeSlots);

    /// <summary>Is a take possible right now (preview armed, no take already in flight)?</summary>
    bool CanTake { get; }

    /// <summary>Take preview to program with the named transition. False when the shell refused
    /// it (the take is async — never block on this task; the UI thread is the caller).</summary>
    Task<bool> TakeAsync(string transition);

    /// <summary>Is this a transition the shell can perform?</summary>
    bool IsKnownTransition(string transition);

    /// <summary>Set the on-air caption text (empty clears it). The caption speaker is untouched.</summary>
    void SetCaption(string text);

    /// <summary>Publish one operator-visible status line (the shell's <c>CommandStatus</c>).
    /// This is TEXT, not show state — the adapter is allowed to call it in shadow mode.</summary>
    void ReportStatus(string line);
}
