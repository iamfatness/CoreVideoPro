namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The ORDER <c>MainWindow.ApplyShowConfigAsync</c> applies a saved show config in (Plan 7b
/// Task 10), lifted out of the window so it has a test seam. <c>MainWindow</c> is not
/// constructible in a unit test (it opens a real WinUI window), and the order is the part of that
/// method with actual failure modes:
///
/// <list type="bullet">
/// <item><b>materialize before restart-engine.</b> The supervisor respawns <c>node</c> against the
/// effective config FILE. Restarting first boots the engine on the previous document, so the
/// operator's Save silently does nothing until the next app launch.</item>
/// <item><b>replace-adapter before restart-engine.</b> A fresh engine issues its first host
/// commands within milliseconds of the restart; an adapter still holding the old look→scene
/// presets would cue the wrong scene — on air.</item>
/// <item><b>validate first.</b> Nothing is written, swapped or restarted for a config the shell
/// already knows is bad.</item>
/// </list>
/// </summary>
public static class OhgConfigApplySteps
{
    public const string Validate = "validate";
    public const string Materialize = "materialize";
    public const string ReplaceAdapter = "replace-adapter";
    public const string RebuildPageViewModel = "rebuild-page-vm";
    public const string RestartEngine = "restart-engine";

    /// <summary>Every step name the window's switch knows how to run. A step in
    /// <see cref="Order"/> that is not here would fall through and silently do nothing.</summary>
    public static IReadOnlyList<string> AllSteps { get; } =
        [Validate, Materialize, ReplaceAdapter, RebuildPageViewModel, RestartEngine];

    private static readonly string[] WithEngine =
        [Validate, Materialize, ReplaceAdapter, RebuildPageViewModel, RestartEngine];

    private static readonly string[] WithoutEngine = [Validate, Materialize];

    /// <summary>The steps to run, in order. With no engine running (no config at launch, or the
    /// host could not be resolved) the config is still checked and written — there is simply
    /// nothing to swap, rebuild or restart, and the settings VM shows the restart-the-app
    /// message instead.</summary>
    public static IReadOnlyList<string> Order(bool engineRunning) => engineRunning ? WithEngine : WithoutEngine;
}
