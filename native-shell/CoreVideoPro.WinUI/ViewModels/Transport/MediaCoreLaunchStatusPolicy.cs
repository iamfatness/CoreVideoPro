using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>What the launch start-up path shows once it has tried the core.</summary>
/// <param name="EngineStatus">The status line to show.</param>
/// <param name="Failed">True only when the core really is unavailable. The status is then also
/// raised as the command status.</param>
/// <param name="QueueSyncRetry">True when the launch sync was skipped and must be delivered by
/// the production-sync retry worker.</param>
public sealed record MediaCoreLaunchStatus(string EngineStatus, bool Failed, bool QueueSyncRetry);

/// <summary>
/// Decides the status the launch start-up path leaves (T1.5, #432).
///
/// Launch starts the core and sends one full production sync. That sync can collide with the
/// bridge's own 250 ms poll, and the supervisor then refuses it with
/// <see cref="MediaCoreSyncInFlightException"/> ("media-core sync in flight; skipped for
/// backpressure"). This used to land in the one catch-all, so a healthy, running core was
/// reported as "Media core unavailable - media-core sync in flight; skipped for backpressure".
/// Nothing overwrote that until Engine On. It was the misleading half of the "core wedge" report.
///
/// A skipped sync is backpressure, not failure. <c>TransportCoordinator.ToggleEngineAsync</c>
/// already treats it that way. The core is ready, and the retry worker delivers the state.
/// Every other exception still means the core is unavailable and is still reported loudly.
/// </summary>
public static class MediaCoreLaunchStatusPolicy
{
    public static MediaCoreLaunchStatus Resolve(Exception? launchFailure, string profileSummary) =>
        launchFailure switch
        {
            null => new($"Media core ready - {profileSummary}", Failed: false, QueueSyncRetry: false),
            MediaCoreSyncInFlightException => new($"Media core ready - {profileSummary}", Failed: false, QueueSyncRetry: true),
            _ => new($"Media core unavailable - {launchFailure.Message}", Failed: true, QueueSyncRetry: false)
        };
}
