using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// T1.8 (#461) close guard — thin forwarders only (strangler rule). The decision lives in
/// <see cref="CloseGuardPolicy"/> and the stop-and-wait in <see cref="OutputShutdownCoordinator"/>;
/// this file just hands them the view model's transport flags, the bridge's latest snapshot, and
/// the existing transport stop commands.
/// </summary>
public sealed partial class StudioViewModel
{
    /// <summary>UI thread. What is live right now, and whether closing should ask first.</summary>
    internal CloseGuardDecision EvaluateCloseGuard() => CloseGuardPolicy.Evaluate(ReadCloseGuardInput());

    /// <summary>UI thread. "Stop and close": stop recording + streaming through the transport, then
    /// wait (bounded, 15 s) for the core to report them finished.</summary>
    internal Task<OutputShutdownOutcome> StopOutputsForCloseAsync() =>
        new OutputShutdownCoordinator(
                readState: ReadCloseGuardInput,
                stopRecording: () => _transportCoordinator.SetRecordingAsync(false),
                stopStreaming: () => _transportCoordinator.SetStreamingAsync(false),
                pollSnapshot: async cancellationToken => _bridge.Running
                    ? await _bridge.PollSnapshotAsync(cancellationToken).ConfigureAwait(true)
                    : null,
                reportProgress: status => OutputStatus = status,
                log: LaunchLog.Write)
            .StopAndWaitAsync();

    private CloseGuardInput ReadCloseGuardInput()
    {
        var running = _bridge.Running;
        NativeMediaCoreStateSnapshot? snapshot = running ? _bridge.LastSnapshot : null;
        return new CloseGuardInput(running, RecordingRequested, Recording, StreamingRequested, Streaming, snapshot);
    }
}
