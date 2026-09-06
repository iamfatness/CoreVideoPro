using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

public static class ZoomJoinReconciliation
{
    private const string JoinTimeout = "Timed out waiting for Zoom meeting join result.";

    // Zoom may emit Joined just after the core's join wait expires. Observe that
    // existing attempt briefly; never issue another join or reinterpret auth errors.
    public static async Task<RawCaptureSnapshot> ObserveLateJoinAsync(
        RawCaptureSnapshot initial, Func<CancellationToken, Task<RawCaptureSnapshot>> poll,
        CancellationToken cancellationToken = default, int graceMs = 5000, int pollIntervalMs = 250)
    {
        if (!IsJoinTimeout(initial)) return initial;
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(Math.Max(1, graceMs));
        try
        {
            while (true)
            {
                var observed = await poll(deadline.Token).WaitAsync(deadline.Token).ConfigureAwait(false);
                if (observed.MeetingState is "in-meeting" or "in_meeting") return observed;
                if (observed.MeetingState == "error" && !IsJoinTimeout(observed)) return observed;
                await Task.Delay(Math.Max(1, pollIntervalMs), deadline.Token).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested) { return initial; }
        catch (Exception) when (!cancellationToken.IsCancellationRequested) { return initial; }
    }

    private static bool IsJoinTimeout(RawCaptureSnapshot snapshot) =>
        snapshot.MeetingState == "error" && snapshot.Warnings?.Any(warning =>
            warning.Contains(JoinTimeout, StringComparison.Ordinal)) == true;
}
