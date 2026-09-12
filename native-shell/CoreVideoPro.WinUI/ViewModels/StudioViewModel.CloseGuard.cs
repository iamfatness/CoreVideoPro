using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// T1.8 (#461) close guard — thin forwarders only (strangler rule). The decision lives in
/// <see cref="CloseGuardPolicy"/> and the stop-and-wait in <see cref="OutputShutdownCoordinator"/>;
/// this file just hands them the view model's transport flags and in-flight guards, the bridge's
/// latest snapshot and core generation, and the existing transport stop commands.
/// </summary>
public sealed partial class StudioViewModel
{
    // True while "Stop and close" is finishing the outputs: starting a new recording or stream
    // is refused (the close owns the transport until the app exits).
    private bool _outputsClosing;

    /// <summary>UI thread. What is live right now, and whether closing should ask first.</summary>
    internal CloseGuardDecision EvaluateCloseGuard() => CloseGuardPolicy.Evaluate(ReadCloseGuardInput());

    /// <summary>UI thread. "Stop and close": stop recording + streaming through the transport, then
    /// wait (bounded, 15 s) for the core to report the close-request sessions finished.</summary>
    internal async Task<OutputShutdownOutcome> StopOutputsForCloseAsync(CloseGuardDecision closeRequest)
    {
        _outputsClosing = true;
        ToggleRecordingCommand.NotifyCanExecuteChanged();
        ToggleStreamingCommand.NotifyCanExecuteChanged();
        OnPropertyChanged(nameof(CanToggleRecording));
        return await new OutputShutdownCoordinator(
                readState: ReadCloseGuardInput,
                stopRecording: () => _transportCoordinator.SetRecordingAsync(false),
                stopStreaming: () => _transportCoordinator.SetStreamingAsync(false),
                reportProgress: status => OutputStatus = status,
                log: LaunchLog.Write)
            .StopAndWaitAsync(closeRequest)
            .ConfigureAwait(true);
    }

    private CloseGuardInput ReadCloseGuardInput()
    {
        var running = _bridge.Running;
        NativeMediaCoreStateSnapshot? snapshot = running ? _bridge.LastSnapshot : null;
        return new CloseGuardInput(
            running,
            RecordingRequested,
            Recording,
            StreamingRequested,
            Streaming,
            snapshot,
            _transportCoordinator.RecordingToggleInFlight,
            _transportCoordinator.StreamToggleInFlight,
            _bridge.Health.RestartCount);
    }
}
