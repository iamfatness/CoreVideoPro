using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>
/// The media-selection half of a Take (the scene half is <c>ITransportHost.CaptureTakeRollback</c>).
/// A Take does more than swap scenes: <c>PromoteProgramMediaRouteToPlayback</c> moves the selection
/// to a clip that went live and marks it playing, and <c>RefreshMediaBinPlaybackIndicators</c> clears
/// the selected clip's playing flag when it left Program. This value is what the Take changed.
/// </summary>
public sealed record MediaSelectionState(
    string? AssetId,
    string? Name,
    string? Path,
    string? Kind,
    bool SupportsPlayback,
    bool Playing,
    string Status);

/// <summary>
/// Decides which media selection stands after a Take is rolled back (T1.3, #430).
///
/// The #286 rollback used to restore the scenes and nothing else. The selection kept whatever
/// the failed Take left there, which is wrong in two ways:
/// <list type="bullet">
/// <item>A clip Y that went live on the failed Take stayed selected and marked playing, although
/// it is off Program again. Its audition kept playing locally with audio, and the status said
/// "Playing Y on Program".</item>
/// <item>A Program clip X that left on the failed Take had its playing flag cleared. The rollback
/// puts X back on Program, still rolling, but the toggle read "Resume Program". Pressing it
/// paused X on air.</item>
/// </list>
///
/// The rule:
/// <list type="bullet">
/// <item>If the selection is still the one the Take left, the pre-Take selection comes back.</item>
/// <item>If the operator moved the selection while the Take's sync was pending, their selection is
/// kept. This matches the scene rollback, which never erases newer edits.</item>
/// <item>A clip on the restored Program then takes its playing flag and status from its real on-air
/// state (routed, and looping or not operator-paused). A saved flag is never trusted for it.
/// Audition playback off Program is local state, so it keeps the saved value.</item>
/// </list>
/// The go-live ledger and the paused set are deliberately NOT rewound (see
/// <c>StudioViewModel.Transport.cs</c>): a clip that rolled on an unconfirmed Take may really have
/// aired.
/// </summary>
public static class TakeMediaSelectionRollback
{
    public static MediaSelectionState Resolve(
        MediaSelectionState beforeTake,
        MediaSelectionState afterTake,
        MediaSelectionState current,
        IReadOnlyList<SourceRoute> restoredProgramRoutes,
        IReadOnlyCollection<string> operatorPausedAssetIds)
    {
        var target = current == afterTake ? beforeTake : current;
        if (string.IsNullOrWhiteSpace(target.AssetId) || !target.SupportsPlayback)
        {
            return target;
        }

        if (!MediaRoutePlaybackService.IsMediaAssetRoutedOnProgram(target.AssetId, restoredProgramRoutes))
        {
            return target;
        }

        var playing = MediaRoutePlaybackService.IsPlayingOnAir(
            target.AssetId,
            isOnProgram: true,
            MediaRoutePlaybackService.IsLoopingKind(target.Kind),
            operatorPausedAssetIds);
        return target with
        {
            Playing = playing,
            Status = playing ? $"Playing {target.Name} on Program" : $"{target.Name} paused on Program"
        };
    }
}
