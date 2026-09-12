using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// The app-close time budget in one place (T1.8, #461), so the core's exit grace can be checked
/// against the shutdown timeout instead of being sized by hope.
///
/// <para><c>MainWindow.ShutdownAsync</c> gives the whole cleanup (control server + view-model
/// disposal, which includes the media-core stop) <see cref="ShutdownTimeout"/>, and a process-level
/// watchdog force-exits at <see cref="ShutdownWatchdog"/>. The core stop on app exit is: close
/// stdin, wait up to <see cref="CoreExitGrace"/> for the core to exit on its own, then kill the tree
/// and wait up to <see cref="MediaCoreSupervisor.KillWaitMilliseconds"/>. Its worst case
/// (<see cref="CoreStopWorstCase"/>, 3.5 s) must leave room inside the 5 s timeout for the rest of
/// the disposal, which is why the grace is 2 s and not longer.</para>
///
/// <para>The stop-and-wait for recording/stream finalization (<see cref="OutputShutdownCoordinator"/>,
/// up to 15 s) happens BEFORE this budget starts: it runs after the operator chooses
/// "Stop and close" and before <c>ShutdownAsync</c> arms the watchdog.</para>
/// </summary>
internal static class ShutdownBudget
{
    internal static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(5);

    internal static readonly TimeSpan ShutdownWatchdog = TimeSpan.FromSeconds(6);

    /// <summary>How long the app-exit stop lets the core exit on its own after stdin closes.</summary>
    internal static readonly TimeSpan CoreExitGrace = TimeSpan.FromSeconds(2);

    /// <summary>Grace that runs out, plus the kill-tree fallback's own wait.</summary>
    internal static TimeSpan CoreStopWorstCase =>
        CoreExitGrace + TimeSpan.FromMilliseconds(MediaCoreSupervisor.KillWaitMilliseconds);
}
