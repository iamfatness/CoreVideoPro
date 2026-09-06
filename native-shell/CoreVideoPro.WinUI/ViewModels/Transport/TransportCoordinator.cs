using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels.Transport;

/// <summary>
/// Owns the transport ORCHESTRATION extracted from the StudioViewModel god object (PR2 of the
/// strangler refactor): the async command bodies for Engine on/off, Take, Record, and Stream,
/// their in-flight guards (<see cref="RecordingToggleInFlight"/> / <see cref="StreamToggleInFlight"/>),
/// and the failed-start rollback / backpressure-retry / sender-proof helpers. Move-only,
/// behavior-parity: the method bodies are the originals, with cross-cluster calls routed through
/// <see cref="ITransportHost"/> instead of <c>this</c> and UI-thread marshalling through
/// <see cref="ITransportDispatcher"/> (the transport commands <c>ConfigureAwait(false)</c> then
/// marshal every bound-property write back to the UI thread — the 0xc000027b rule, preserved
/// exactly). Pure status/rollback/validation logic still lives in the static
/// <see cref="TransportStatusFormatter"/> (PR1). StudioViewModel keeps the generated
/// <c>[RelayCommand]</c> objects and forwards their bodies here, so XAML x:Bind and the external
/// <c>NotifyCanExecuteChanged</c> pokes are unchanged.
///
/// This type is independently constructible (via a fake <see cref="IMediaCoreBridge"/> + fake
/// <see cref="ITransportHost"/> + inline <see cref="ITransportDispatcher"/>), which is what
/// carries the first-ever characterization tests of transport orchestration.
/// </summary>
public sealed class TransportCoordinator
{
    private readonly IMediaCoreBridge _bridge;
    private readonly ITransportHost _host;
    private readonly ITransportDispatcher _dispatcher;
    private readonly int _recordingSyncRetryAttempts;
    private readonly int _recordingSyncRetryDelayMs;

    private bool _recordingToggleInFlight;
    private bool _streamToggleInFlight;
    private bool _takeInFlight;

    public TransportCoordinator(
        IMediaCoreBridge bridge,
        ITransportHost host,
        ITransportDispatcher dispatcher,
        int recordingSyncRetryAttempts = 120,
        int recordingSyncRetryDelayMs = 250)
    {
        _bridge = bridge;
        _host = host;
        _dispatcher = dispatcher;
        _recordingSyncRetryAttempts = Math.Max(1, recordingSyncRetryAttempts);
        _recordingSyncRetryDelayMs = Math.Max(1, recordingSyncRetryDelayMs);
    }

    /// <summary>In-flight guard for the Record toggle. StudioViewModel's
    /// <c>CanToggleRecording</c> predicate reads it (was the private <c>_recordingToggleInFlight</c>).</summary>
    public bool RecordingToggleInFlight => _recordingToggleInFlight;

    /// <summary>In-flight guard for the Stream toggle (was the private <c>_streamToggleInFlight</c>).</summary>
    public bool StreamToggleInFlight => _streamToggleInFlight;

    // The top-right toggle controls the Zoom CAPTURE SUBSCRIPTION, not the media core.
    // The media core / compositor is always on (started on join, stopped on leave) so
    // the program/preview surfaces always show the live compositor output. Toggling here
    // only subscribes/unsubscribes the raw Zoom ingest spine sync.
    public async Task ToggleEngineAsync()
    {
        try
        {
            if (_host.ZoomCaptureSubscribed)
            {
                _host.UnsubscribeZoomCapture("Zoom capture paused");
            }
            else
            {
                // The media core should already be running from launch. If it died, bring it back up rather than dead-ending.
                // ConfigureAwait(true): this is a UI [RelayCommand]; the whole
                // continuation below sets x:Bound properties (EngineStatus,
                // ZoomCaptureSubscribed) + RefreshSurfaceBindings, so it MUST resume on
                // the UI thread or it fail-fasts (CoreMessagingXP 0xc000027b). This was
                // the recurring Engine-On crash.
                await _host.EnsureMediaCoreRunningAsync("Restarting media core...").ConfigureAwait(true);

                if (!_bridge.Running)
                {
                    _host.EngineStatus = "Media core is unavailable — rejoin the meeting.";
                    return;
                }

                _host.EngineStatus = "Requesting Zoom capture…";
                _bridge.ConfigureZoomSpineSync(_host.BuildSpinePayload);
                _host.ZoomCaptureSubscribed = true;
                _host.NotifySurfacesCaptureSubscribed(true, _bridge.Profile?.Renderer);
                _host.NotifySurfacesPreviewParticipant(_host.SelectedParticipantId);
                _host.EngineStatus = $"Capture live — {_bridge.ProfileSummary}";
                _host.RefreshSdkReadiness();
                try
                {
                    await _host.SyncActiveSceneAsync().ConfigureAwait(true);
                }
                catch (MediaCoreSyncInFlightException)
                {
                    // Transient backpressure: another sync was already running when
                    // the operator flipped Engine On. Capture is enabled and the
                    // spine/periodic sync will apply the active scene shortly — this
                    // is NOT a toggle failure, so keep capture on.
                    _host.EngineStatus = $"Capture live — {_bridge.ProfileSummary}";
                }
                _host.RefreshSurfaceBindings();
                _host.RefreshTransportState();
                _host.NotifyRecordingCommandCanExecuteChanged();
            }
        }
        catch (MediaCoreSyncInFlightException)
        {
            // Backpressure during toggle — non-fatal; leave capture enabled.
            _host.EngineStatus = "Capture starting…";
            _host.RefreshTransportState();
            _host.NotifyRecordingCommandCanExecuteChanged();
        }
        catch (Exception ex)
        {
            _host.ZoomCaptureSubscribed = false;
            _host.NotifySurfacesCaptureSubscribed(false, null);
            _host.EngineStatus = ex.Message;
            _host.RefreshSdkReadiness();
            _host.RefreshSurfaceBindings();
            _host.RefreshTransportState();
            _host.NotifyRecordingCommandCanExecuteChanged();
        }
    }

    public async Task<TakeResult> TakeAsync()
    {
        if (_takeInFlight) return TakeResult.Failed("A Take is already in progress.");
        _takeInFlight = true;
        try
        {
            var previousProgramSceneId = _host.ActiveSceneId;
            var takenSceneId = _host.PreviewSceneId;
            if (!_host.IsSceneAvailable(previousProgramSceneId) || !_host.IsSceneAvailable(takenSceneId))
            {
                const string unavailable = "Take unavailable - select an available Preview scene.";
                _host.CommandStatus = unavailable;
                return TakeResult.Failed(unavailable);
            }

            var sealRollback = _host.CaptureTakeRollback();
            _host.BeginTakeMutation();
            try
            {
                if (previousProgramSceneId == takenSceneId && _host.HasPendingPreviewChanges(takenSceneId))
                    _host.CopyPreviewRoutesToScene(takenSceneId);
                else
                {
                    _host.ActiveSceneId = takenSceneId;
                    _host.PreviewSceneId = previousProgramSceneId;
                }
                _host.IncrementProgramMediaPlaybackTakeVersion();
                _host.PromoteProgramMediaRouteToPlayback();
                _host.RefreshPreviewRoutingState();
            }
            finally { _host.EndTakeMutation(); }
            var rollback = sealRollback();

            if (!_bridge.Running)
            {
                const string offline = "Program selected locally; Take was not sent because the media core is offline.";
                _host.OutputStatus = offline;
                return TakeResult.Failed(offline);
            }
            // Swap once. Backpressure retries the resulting state, never the toggle.
            for (var attempt = 0; attempt < _recordingSyncRetryAttempts; attempt++)
            {
                try
                {
                    await _host.SyncActiveSceneAsync("take").ConfigureAwait(true);
                    _host.OutputStatus = "Program updated";
                    return TakeResult.Success;
                }
                catch (MediaCoreSyncInFlightException)
                {
                    _host.CommandStatus = "Take waiting for media core...";
                    if (attempt + 1 < _recordingSyncRetryAttempts)
                        await Task.Delay(_recordingSyncRetryDelayMs).ConfigureAwait(true);
                }
                catch (Exception ex)
                {
                    var restored = rollback();
                    if (restored) _host.RequestTakeReconciliation();
                    var failure = restored
                        ? $"Take was not confirmed; previous local Program restored. Verify live output before retrying. {ex.Message}"
                        : $"Take was not confirmed; newer local edits preserved. Verify live output. {ex.Message}";
                    _host.CommandStatus = failure;
                    return TakeResult.Failed(failure);
                }
            }
            var rolledBack = rollback();
            if (rolledBack) _host.RequestTakeReconciliation();
            var exhausted = rolledBack
                ? "Take could not reach the busy media core. Previous local Program restored; retry Take."
                : "Take could not reach the busy media core; newer local edits preserved.";
            _host.CommandStatus = exhausted;
            return TakeResult.Failed(exhausted);
        }
        finally { _takeInFlight = false; }
    }

    public Task ToggleRecordingAsync() => SetRecordingAsync(!_host.Recording);

    public async Task SetRecordingAsync(bool starting)
    {
        if (_recordingToggleInFlight)
        {
            return;
        }

        _recordingToggleInFlight = true;
        _host.NotifyRecordingCommandCanExecuteChanged();
        var previousRecording = _host.Recording;
        LaunchLog.Write(
            $"recording: toggle requested action={(starting ? "start" : "stop")} " +
            $"format={_host.RecordingLogFormat} " +
            $"bitrate={_host.RecordingLogBitrateMbps:0.0}Mbps " +
            $"codec={_host.RecordingVideoCodec}");

        // ISO-4 (spec §6): disk pre-flight BEFORE arming, not a mid-show surprise.
        // Genuinely-insufficient space hard-blocks the start; low space warns loudly (a
        // persistent RecordingDiskWarning that survives the "start requested" status) but
        // proceeds; an unmeasurable volume never blocks (warn-not-silent). The `finally`
        // resets _recordingToggleInFlight on the early return.
        if (starting)
        {
            _host.RecordingDiskWarning = null;
            if (_host.TryEvaluateRecordingDiskPreflight(out var preflight))
            {
                if (preflight.ShouldBlock)
                {
                    LaunchLog.Write($"recording: start BLOCKED by disk pre-flight — {preflight.Message}");
                    _recordingToggleInFlight = false;
                    _host.NotifyRecordingCommandCanExecuteChanged();
                    _host.OutputStatus = preflight.Message;
                    _host.OutputSessionStatus = _host.OutputStatus;
                    _host.RefreshOutputStatus();
                    return;
                }

                if (preflight.ShouldWarn)
                {
                    LaunchLog.Write($"recording: disk pre-flight WARNING — {preflight.Message}");
                    _host.RecordingDiskWarning = preflight.Message;
                }
            }
        }
        else
        {
            _host.RecordingDiskWarning = null;
        }

        _host.Recording = starting;
        _host.RefreshOutputStatus();

        try
        {
            await _host.EnsureMediaCoreRunningAsync(starting ? "Starting media core for recording..." : "Updating media core...").ConfigureAwait(false);
            var snapshot = await _host.SyncActiveSceneAsync().ConfigureAwait(false);
            if (starting && TransportStatusFormatter.TryFormatRecordingStartHealthFailure(snapshot, out var healthFailureStatus))
            {
                _dispatcher.RunOnUiThread(() =>
                {
                    _host.Recording = previousRecording;
                    _host.RefreshOutputStatus();
                    _host.OutputStatus = healthFailureStatus;
                    _host.OutputSessionStatus = _host.OutputStatus;
                });
                _ = SyncFailedRecordingStartRollbackAsync(healthFailureStatus);
                return;
            }

            _dispatcher.RunOnUiThread(() =>
            {
                _host.OutputStatus = starting ? "Recording start requested." : "Recording stop requested — finalizing.";
                _host.OutputSessionStatus = _host.OutputStatus;
            });
        }
        catch (MediaCoreSyncInFlightException)
        {
            _dispatcher.RunOnUiThread(() =>
            {
                _host.OutputStatus = starting ? "Recording starting - applying..." : "Recording stopping - applying...";
                _host.OutputSessionStatus = _host.OutputStatus;
            });
            LaunchLog.Write($"recording: {(starting ? "start" : "stop")} deferred because media core sync is in flight");
            // A multi-ISO start can legitimately hold the core sync for several seconds
            // while its writers come up. Await the retry so the Record command stays
            // guarded; fire-and-forget let a second operator click invert the requested
            // state before the first stop had reached the core.
            await RetryRecordingSyncAsync(starting).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            var action = starting ? "start" : "stop";
            var failureStatus = TransportStatusFormatter.FormatRecordingFailureStatus(action, ex);
            LaunchLog.Write($"recording: {action} failed {ex.GetType().Name}: {ex.Message}");
            _dispatcher.RunOnUiThread(() =>
            {
                // Starting may safely roll back. Stopping is a safety intent: never
                // restore Recording=true merely because the core was momentarily busy.
                // The periodic production sync will continue carrying Recording=false.
                _host.Recording = starting ? previousRecording : false;
                _host.RefreshOutputStatus();
                _host.OutputStatus = failureStatus;
                _host.OutputSessionStatus = _host.OutputStatus;
            });

            if (starting && !previousRecording)
            {
                _ = SyncFailedRecordingStartRollbackAsync(failureStatus);
            }
        }
        finally
        {
            _dispatcher.RunOnUiThread(() =>
            {
                _recordingToggleInFlight = false;
                _host.NotifyRecordingCommandCanExecuteChanged();
            });
        }
    }

    private async Task SyncFailedRecordingStartRollbackAsync(string failureStatus)
    {
        for (var attempt = 0; attempt < 4; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                await _host.SyncActiveSceneAsync().ConfigureAwait(false);
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Still busy; retry shortly.
            }
            catch (Exception ex)
            {
                var rollbackFailure = TransportStatusFormatter.NormalizeStreamingFailureDetail(ex.Message);
                _dispatcher.RunOnUiThread(() =>
                {
                    _host.OutputStatus = $"{failureStatus} Recording cleanup also failed: {rollbackFailure}";
                    _host.OutputSessionStatus = _host.OutputStatus;
                });
                return;
            }
        }

        _dispatcher.RunOnUiThread(() =>
        {
            _host.OutputStatus = $"{failureStatus} Recording cleanup is still waiting for media core.";
            _host.OutputSessionStatus = _host.OutputStatus;
        });
    }

    private async Task RetryRecordingSyncAsync(bool starting)
    {
        for (var attempt = 0; attempt < _recordingSyncRetryAttempts; attempt++)
        {
            await Task.Delay(_recordingSyncRetryDelayMs).ConfigureAwait(false);
            try
            {
                var snapshot = await _host.SyncActiveSceneAsync().ConfigureAwait(false);
                if (starting && TransportStatusFormatter.TryFormatRecordingStartHealthFailure(snapshot, out var healthFailureStatus))
                {
                    _dispatcher.RunOnUiThread(() =>
                    {
                        _host.Recording = TransportStatusFormatter.ResolveRecordingStateAfterFailedRetry(starting);
                        _host.RefreshOutputStatus();
                        _host.OutputStatus = healthFailureStatus;
                        _host.OutputSessionStatus = _host.OutputStatus;
                    });
                    _ = SyncFailedRecordingStartRollbackAsync(healthFailureStatus);
                    return;
                }

                _dispatcher.RunOnUiThread(() =>
                {
                    _host.OutputStatus = starting ? "Recording start requested." : "Recording stop requested — finalizing.";
                    _host.OutputSessionStatus = _host.OutputStatus;
                    _host.RefreshOutputStatus();
                });
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Still busy; try again shortly.
            }
            catch (Exception ex)
            {
                var failureStatus = TransportStatusFormatter.FormatRecordingFailureStatus(starting ? "start" : "stop", ex);
                _dispatcher.RunOnUiThread(() =>
                {
                    _host.Recording = starting
                        ? TransportStatusFormatter.ResolveRecordingStateAfterFailedRetry(true)
                        : false;
                    _host.RefreshOutputStatus();
                    _host.OutputStatus = failureStatus;
                    _host.OutputSessionStatus = _host.OutputStatus;
                });

                if (starting)
                {
                    _ = SyncFailedRecordingStartRollbackAsync(failureStatus);
                }
                return;
            }
        }

        var exhaustedStatus = starting
            ? TransportStatusFormatter.FormatRecordingSyncRetryExhaustedStatus(true)
            : "Recording stopping - media core is busy; stop remains armed.";
        _dispatcher.RunOnUiThread(() =>
        {
            _host.Recording = starting
                ? TransportStatusFormatter.ResolveRecordingStateAfterFailedRetry(true)
                : false;
            _host.RefreshOutputStatus();
            _host.OutputStatus = exhaustedStatus;
            _host.OutputSessionStatus = _host.OutputStatus;
        });

        if (starting)
        {
            _ = SyncFailedRecordingStartRollbackAsync(exhaustedStatus);
        }
    }

    public Task ToggleStreamingAsync() => SetStreamingAsync(!_host.Streaming);

    public async Task SetStreamingAsync(bool starting)
    {
        if (_streamToggleInFlight)
        {
            return;
        }

        _streamToggleInFlight = true;
        _host.NotifyStreamingCommandCanExecuteChanged();
        var requestedDestinations = _host.BuildSelectedStreamDestinations(validatedOnly: true);
        LaunchLog.Write(
            $"stream: toggle requested action={(starting ? "start" : "stop")} " +
            $"rtmp={_host.StreamRtmpEnabled} ndi={_host.StreamNdiEnabled} srt={_host.StreamSrtEnabled} " +
            $"destinations={_host.FormatStreamDestinationTelemetry(requestedDestinations)} " +
            $"bitrate={_host.StreamLogBitrateMbps:0.0}Mbps " +
            $"codec={_host.StreamVideoCodec} encoder={_host.StreamEncoderMode}");
        try
        {
            if (starting && _host.ValidateStreamDestinations() is { Length: > 0 } validationError)
            {
                var failureStatus = TransportStatusFormatter.FormatStreamingFailureStatus("start", new InvalidOperationException(validationError));
                LaunchLog.Write($"stream: start blocked ({validationError})");
                _host.OutputStatus = failureStatus;
                _host.OutputSessionStatus = failureStatus;
                return;
            }

            var previousStreaming = _host.Streaming;
            _host.Streaming = starting;
            _host.RefreshOutputStatus();

            try
            {
                await _host.EnsureMediaCoreRunningAsync(starting ? "Starting media core for stream..." : "Updating media core...").ConfigureAwait(false);
                if (starting && _host.ValidateStreamDestinations() is { Length: > 0 } runtimeValidationError)
                {
                    var failureStatus = TransportStatusFormatter.FormatStreamingFailureStatus("start", new InvalidOperationException(runtimeValidationError));
                    LaunchLog.Write($"stream: start blocked after media-core profile ({runtimeValidationError})");
                    _dispatcher.RunOnUiThread(() =>
                    {
                        _host.Streaming = previousStreaming;
                        _host.RefreshOutputStatus();
                        _host.OutputStatus = failureStatus;
                        _host.OutputSessionStatus = _host.OutputStatus;
                    });
                    return;
                }

                var snapshot = await _host.SyncActiveSceneAsync(starting ? "stream-start" : "stream-stop").ConfigureAwait(false);
                if (starting && TransportStatusFormatter.TryFormatStreamingStartHealthFailure(snapshot, out var healthFailureStatus))
                {
                    LaunchLog.Write($"stream: start failed health proof {healthFailureStatus}");
                    _dispatcher.RunOnUiThread(() =>
                    {
                        _host.Streaming = previousStreaming;
                        _host.RefreshOutputStatus();
                        _host.OutputStatus = healthFailureStatus;
                        _host.OutputSessionStatus = _host.OutputStatus;
                    });
                    _ = SyncFailedStreamStartRollbackAsync(healthFailureStatus);
                    return;
                }

                if (starting)
                {
                    var proof = await WaitForStreamingStartProofAsync(requestedDestinations, snapshot).ConfigureAwait(false);
                    if (!string.IsNullOrWhiteSpace(proof.FailureStatus))
                    {
                        LaunchLog.Write($"stream: start failed sender proof {proof.FailureStatus}");
                        _dispatcher.RunOnUiThread(() =>
                        {
                            _host.Streaming = previousStreaming;
                            _host.RefreshOutputStatus();
                            _host.OutputStatus = proof.FailureStatus;
                            _host.OutputSessionStatus = _host.OutputStatus;
                        });
                        _ = SyncFailedStreamStartRollbackAsync(proof.FailureStatus);
                        return;
                    }
                }

                _dispatcher.RunOnUiThread(() =>
                {
                    _host.OutputStatus = starting ? "Streaming start requested." : "Streaming stopped.";
                    _host.OutputSessionStatus = _host.OutputStatus;
                });
            }
            catch (MediaCoreSyncInFlightException)
            {
                // A background sync was mid-flight, so this toggle's sync was skipped
                // for backpressure. The requested Streaming state is already set, so
                // keep it and let a follow-up sync apply it instead of rolling back and
                // reporting a hard failure (this is the confusing "media-..." error).
                _dispatcher.RunOnUiThread(() =>
                {
                    _host.OutputStatus = starting ? "Streaming starting - applying..." : "Streaming stopping - applying...";
                    _host.OutputSessionStatus = _host.OutputStatus;
                });
                LaunchLog.Write($"stream: {(starting ? "start" : "stop")} deferred because media core sync is in flight");
                _ = RetryStreamSyncAsync(starting);
            }
            catch (Exception ex)
            {
                var action = starting ? "start" : "stop";
                var failureStatus = TransportStatusFormatter.FormatStreamingFailureStatus(action, ex);
                LaunchLog.Write($"stream: {action} failed {ex.GetType().Name}: {ex.Message}");
                _dispatcher.RunOnUiThread(() =>
                {
                    // A failed Stop must not let the next state sync re-arm the sender.
                    _host.Streaming = starting && previousStreaming;
                    _host.RefreshOutputStatus();
                    _host.OutputStatus = failureStatus;
                    _host.OutputSessionStatus = _host.OutputStatus;
                });

                if (starting && !previousStreaming)
                {
                    _ = SyncFailedStreamStartRollbackAsync(failureStatus);
                }
            }
        }
        finally
        {
            _dispatcher.RunOnUiThread(() =>
            {
                _streamToggleInFlight = false;
                _host.NotifyStreamingCommandCanExecuteChanged();
            });
        }
    }

    private async Task SyncFailedStreamStartRollbackAsync(string failureStatus)
    {
        LaunchLog.Write($"stream: rollback after failed start {failureStatus}");
        for (var attempt = 0; attempt < 4; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                await _host.SyncActiveSceneAsync().ConfigureAwait(false);
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Wait for the active sync to clear, then retry the explicit stop.
            }
            catch (Exception ex)
            {
                var rollbackFailure = TransportStatusFormatter.NormalizeStreamingFailureDetail(ex.Message);
                _dispatcher.RunOnUiThread(() =>
                {
                    _host.OutputStatus = $"{failureStatus} Stream cleanup also failed: {rollbackFailure}";
                    _host.OutputSessionStatus = _host.OutputStatus;
                });
                return;
            }
        }

        _dispatcher.RunOnUiThread(() =>
        {
            _host.OutputStatus = $"{failureStatus} Stream cleanup is still waiting for media core.";
            _host.OutputSessionStatus = _host.OutputStatus;
        });
    }

    // Re-applies the production sync after a stream toggle was skipped for
    // backpressure, so the requested Streaming state actually reaches the media
    // core once the in-flight sync clears. Retries a few times, then gives up
    // quietly (the periodic sync will still converge).
    private async Task RetryStreamSyncAsync(bool starting)
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            await Task.Delay(150).ConfigureAwait(false);
            try
            {
                var snapshot = await _host.SyncActiveSceneAsync().ConfigureAwait(false);
                if (starting && TransportStatusFormatter.TryFormatStreamingStartHealthFailure(snapshot, out var healthFailureStatus))
                {
                    LaunchLog.Write($"stream: retry start failed health proof {healthFailureStatus}");
                    _dispatcher.RunOnUiThread(() =>
                    {
                        _host.Streaming = TransportStatusFormatter.ResolveStreamingStateAfterFailedRetry(starting);
                        _host.RefreshOutputStatus();
                        _host.OutputStatus = healthFailureStatus;
                        _host.OutputSessionStatus = _host.OutputStatus;
                    });
                    _ = SyncFailedStreamStartRollbackAsync(healthFailureStatus);
                    return;
                }

                if (starting)
                {
                    var requestedDestinations = _host.BuildSelectedStreamDestinations(validatedOnly: true);
                    var proof = await WaitForStreamingStartProofAsync(requestedDestinations, snapshot).ConfigureAwait(false);
                    if (!string.IsNullOrWhiteSpace(proof.FailureStatus))
                    {
                        LaunchLog.Write($"stream: retry start failed sender proof {proof.FailureStatus}");
                        _dispatcher.RunOnUiThread(() =>
                        {
                            _host.Streaming = TransportStatusFormatter.ResolveStreamingStateAfterFailedRetry(starting);
                            _host.RefreshOutputStatus();
                            _host.OutputStatus = proof.FailureStatus;
                            _host.OutputSessionStatus = _host.OutputStatus;
                        });
                        _ = SyncFailedStreamStartRollbackAsync(proof.FailureStatus);
                        return;
                    }
                }

                _dispatcher.RunOnUiThread(() =>
                {
                    _host.OutputStatus = starting ? "Streaming start requested." : "Streaming stopped.";
                    _host.OutputSessionStatus = _host.OutputStatus;
                    _host.RefreshOutputStatus();
                });
                return;
            }
            catch (MediaCoreSyncInFlightException)
            {
                // Still busy; try again shortly.
            }
            catch (Exception ex)
            {
                var failureStatus = TransportStatusFormatter.FormatStreamingFailureStatus(starting ? "start" : "stop", ex);
                _dispatcher.RunOnUiThread(() =>
                {
                    _host.Streaming = TransportStatusFormatter.ResolveStreamingStateAfterFailedRetry(starting);
                    _host.RefreshOutputStatus();
                    _host.OutputStatus = failureStatus;
                    _host.OutputSessionStatus = _host.OutputStatus;
                });

                if (starting)
                {
                    _ = SyncFailedStreamStartRollbackAsync(failureStatus);
                }
                return;
            }
        }

        var exhaustedStatus = TransportStatusFormatter.FormatStreamSyncRetryExhaustedStatus(starting);
        _dispatcher.RunOnUiThread(() =>
        {
            _host.Streaming = TransportStatusFormatter.ResolveStreamingStateAfterFailedRetry(starting);
            _host.RefreshOutputStatus();
            _host.OutputStatus = exhaustedStatus;
            _host.OutputSessionStatus = _host.OutputStatus;
        });

        if (starting)
        {
            _ = SyncFailedStreamStartRollbackAsync(exhaustedStatus);
        }
    }

    private async Task<(NativeMediaCoreStateSnapshot Snapshot, string? FailureStatus)> WaitForStreamingStartProofAsync(
        IReadOnlyList<string> requestedDestinations,
        NativeMediaCoreStateSnapshot initialSnapshot)
    {
        var snapshot = initialSnapshot;
        for (var attempt = 0; attempt < 6; attempt++)
        {
            if (TransportStatusFormatter.TryFormatStreamingStartHealthFailure(snapshot, out var healthFailureStatus))
            {
                return (snapshot, healthFailureStatus);
            }

            if (TransportStatusFormatter.IsStreamingStartProven(snapshot, requestedDestinations))
            {
                return (snapshot, null);
            }

            await Task.Delay(200).ConfigureAwait(false);
            snapshot = await _bridge.PollSnapshotAsync().ConfigureAwait(false);
        }

        return TransportStatusFormatter.TryFormatStreamingStartNoSenderFailure(snapshot, requestedDestinations, out var failureStatus)
            ? (snapshot, failureStatus)
            : (snapshot, null);
    }
}
