using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

public sealed partial class TransportCoordinator
{
    private sealed record PendingLayoutTake(GraphicsLayoutTakeRequest Request, string PreviousProgramSceneId, Func<bool> Rollback)
    {
        public bool SubmitNeeded { get; set; } = true;
    }
    private PendingLayoutTake? _guardedTake;
    public bool GuardedTakePending => _guardedTake is not null;

    private async Task<TakeResult> ResolveGuardedTakeAsync(PendingLayoutTake pending)
    {
        var request = pending.Request;
        NativeMediaCoreStateSnapshot? latest = null;
        for (var retry = 0; retry < 16; retry++)
        {
            var sending = pending.SubmitNeeded;
            pending.SubmitNeeded = false;
            NativeMediaCoreStateSnapshot? observation = null;
            try
            {
                observation = sending
                    ? await _host.SyncGuardedTakeAsync(request)
                    : await _host.ObserveGuardedTakeAsync();
            }
            catch (MediaCoreSyncInFlightException)
            {
                // The scheduler declined the command before it could reach the core.
                pending.SubmitNeeded = sending;
            }
            catch (Exception ex) when (ex is InvalidOperationException or IOException or TimeoutException or OperationCanceledException)
            {
                // The request may have been committed. Read the operation receipt before doing anything else.
            }

            latest = observation ?? latest;
            if (observation?.Graphics is { } graphics)
            {
                if (graphics.CoreEpoch != request.Guard.CoreEpoch)
                    return await FinishGuardedTakeAsync(pending, "Core restarted. Cue the Shot Layout again.");
                var receipt = graphics.LayoutTakeReceipts.FirstOrDefault(item => item.OperationId == request.OperationId);
                if (receipt?.Disposition == "committed")
                    return await FinishGuardedTakeAsync(pending, null);
                if (receipt?.Disposition == "refused")
                    return await FinishGuardedTakeAsync(pending, $"Shot Layout Take refused. {receipt.Reason}");
                // A cold source is prepared by the render tick. The core retained the original batch.
                pending.SubmitNeeded = receipt?.Disposition == "ready";
            }

            if (retry < 15)
                await Task.Delay(Math.Min(125, _recordingSyncRetryDelayMs));
        }

        if (latest?.Graphics is { } state && state.CoreEpoch == request.Guard.CoreEpoch &&
            state.LayoutTakeReceipts.All(receipt => receipt.OperationId != request.OperationId))
            return await FinishGuardedTakeAsync(pending, "The core could not confirm the Take. Cue the Shot Layout again.",
                restore: latest.SceneId == pending.PreviousProgramSceneId);

        const string unconfirmed = "Shot Layout Take is awaiting confirmation. Retry Take to check the same operation.";
        _host.CommandStatus = unconfirmed;
        return TakeResult.Failed(unconfirmed);
    }

    private async Task<TakeResult> FinishGuardedTakeAsync(PendingLayoutTake pending, string? refusal, bool restore = true)
    {
        if (refusal is not null && restore) pending.Rollback();
        _guardedTake = null;
        await _host.FinishGuardedTakeAsync(pending.Request.OperationId, refusal is null);
        _host.NotifyGuardedTakeChanged();
        if (refusal is null)
        {
            _host.OutputStatus = "Program updated";
            return TakeResult.Success;
        }
        _host.CommandStatus = refusal;
        return TakeResult.Failed(refusal);
    }
}
