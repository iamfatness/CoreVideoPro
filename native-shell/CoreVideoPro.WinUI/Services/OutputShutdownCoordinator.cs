using System.Diagnostics;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

internal enum OutputShutdownOutcome
{
    /// <summary>Nothing was recording or streaming when the stop began, so nothing was sent.</summary>
    NothingLive,

    /// <summary>The core reported every destination completed / idle.</summary>
    Finished,

    /// <summary>Everything stopped, but a destination ended failed or interrupted.</summary>
    FinishedWithFailure,

    /// <summary>The core is gone, so there is nothing left to wait for.</summary>
    CoreUnavailable,

    /// <summary>The outputs did not finish within <see cref="OutputShutdownCoordinator.FinishTimeout"/>.</summary>
    TimedOut
}

/// <summary>
/// "Stop and close" (T1.8, #461): stop recording and streaming through the EXISTING transport
/// commands, then wait for the core to say the files are finished before the app shuts the core
/// down. The evidence is the destination lifecycle in the core's snapshots
/// (<see cref="CloseGuardPolicy.EvaluateSettled"/>) — a stop acknowledgement is not evidence, because
/// the core's Stop reports that stopping has BEGUN, and the moov atom is written later, on the
/// writer thread. The wait is bounded by <see cref="FinishTimeout"/>; on timeout it logs loudly and
/// lets the close proceed (never a hang).
///
/// <para><b>Threading.</b> Construct and call it on the UI thread. The transport stop commands write
/// bound properties before their first await, and every await here resumes on the caller's context
/// (no ConfigureAwait(false)), so a re-sent stop also runs on the UI thread (the 0xc000027b rule).</para>
///
/// <para>Every dependency is injected (the stop commands, the snapshot poll, the clock, the delay)
/// so the whole wait is testable without a core or a window.</para>
/// </summary>
internal sealed class OutputShutdownCoordinator
{
    /// <summary>The longest the app waits for recording / streams to finish before closing anyway.</summary>
    internal static readonly TimeSpan FinishTimeout = TimeSpan.FromSeconds(15);

    internal static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(250);

    // A stop that has not landed (a toggle was in flight, so the transport ignored ours) is re-sent
    // at most this often. The transport's own in-flight guards keep re-sends from piling up.
    internal static readonly TimeSpan StopResendInterval = TimeSpan.FromSeconds(1);

    internal const string ProgressFinishingRecording = "Finishing the recording before closing…";
    internal const string ProgressStoppingStreams = "Stopping streams before closing…";

    private readonly Func<CloseGuardInput> _readState;
    private readonly Func<Task> _stopRecording;
    private readonly Func<Task> _stopStreaming;
    private readonly Func<CancellationToken, Task<NativeMediaCoreStateSnapshot?>> _pollSnapshot;
    private readonly Action<string> _reportProgress;
    private readonly Action<string> _log;
    private readonly Func<TimeSpan> _elapsed;
    private readonly Func<TimeSpan, Task> _delay;
    private readonly TimeSpan _timeout;

    /// <param name="readState">The shell's flags + latest snapshot (for "was anything live?").</param>
    /// <param name="stopRecording">The existing transport stop (SetRecordingAsync(false)).</param>
    /// <param name="stopStreaming">The existing transport stop (SetStreamingAsync(false)).</param>
    /// <param name="pollSnapshot">A fresh core snapshot; null or <see cref="InvalidOperationException"/>
    /// when the core is not running.</param>
    /// <param name="reportProgress">Writes the operator-visible status line.</param>
    /// <param name="log">The launch log.</param>
    /// <param name="elapsed">Monotonic time since construction; defaults to a Stopwatch.</param>
    /// <param name="delay">Waits between polls; defaults to Task.Delay.</param>
    /// <param name="timeout">Defaults to <see cref="FinishTimeout"/>.</param>
    internal OutputShutdownCoordinator(
        Func<CloseGuardInput> readState,
        Func<Task> stopRecording,
        Func<Task> stopStreaming,
        Func<CancellationToken, Task<NativeMediaCoreStateSnapshot?>> pollSnapshot,
        Action<string> reportProgress,
        Action<string> log,
        Func<TimeSpan>? elapsed = null,
        Func<TimeSpan, Task>? delay = null,
        TimeSpan? timeout = null)
    {
        _readState = readState;
        _stopRecording = stopRecording;
        _stopStreaming = stopStreaming;
        _pollSnapshot = pollSnapshot;
        _reportProgress = reportProgress;
        _log = log;
        if (elapsed is null)
        {
            var stopwatch = Stopwatch.StartNew();
            elapsed = () => stopwatch.Elapsed;
        }

        _elapsed = elapsed;
        _delay = delay ?? (interval => Task.Delay(interval));
        _timeout = timeout ?? FinishTimeout;
    }

    internal async Task<OutputShutdownOutcome> StopAndWaitAsync()
    {
        var decision = CloseGuardPolicy.Evaluate(_readState());
        if (!decision.ShouldAsk)
        {
            _log("shutdown: no recording or stream is live; nothing to finish before closing");
            return OutputShutdownOutcome.NothingLive;
        }

        var started = _elapsed();
        var deadline = started + _timeout;
        _log($"shutdown: stopping outputs before close ({decision.Description}); waiting up to {_timeout.TotalSeconds:0}s for them to finish");
        TryReportProgress(decision.RecordingActive ? ProgressFinishingRecording : ProgressStoppingStreams);

        SendStops(decision.RecordingActive, decision.StreamingActive, "stop");
        var lastStopSentAt = _elapsed();

        var lastDetail = string.Empty;
        while (true)
        {
            NativeMediaCoreStateSnapshot? snapshot;
            var pollBound = deadline - _elapsed();
            using var pollCancellation = pollBound > TimeSpan.Zero
                ? new CancellationTokenSource(pollBound)
                : new CancellationTokenSource();
            try
            {
                snapshot = await _pollSnapshot(pollCancellation.Token).ConfigureAwait(true);
            }
            catch (InvalidOperationException ex) when (!_readState().CoreRunning)
            {
                _log($"shutdown: media core is not running while outputs finish ({ex.Message}); closing");
                return OutputShutdownOutcome.CoreUnavailable;
            }
            catch (Exception ex)
            {
                // Transient (sync in flight, a slow response): keep waiting inside the bound.
                _log($"shutdown: snapshot poll while outputs finish failed ({ex.GetType().Name}: {ex.Message}); retrying");
                snapshot = null;
            }

            if (snapshot is null && !_readState().CoreRunning)
            {
                _log("shutdown: media core is not running while outputs finish; closing");
                return OutputShutdownOutcome.CoreUnavailable;
            }

            var settled = CloseGuardPolicy.EvaluateSettled(snapshot);
            var waitedMs = (_elapsed() - started).TotalMilliseconds;
            switch (settled.State)
            {
                case OutputSettleState.Settled:
                    _log($"shutdown: outputs finished after {waitedMs:0}ms ({settled.Detail})");
                    return OutputShutdownOutcome.Finished;
                case OutputSettleState.SettledWithFailure:
                    _log($"shutdown: outputs stopped after {waitedMs:0}ms but a destination did not complete ({settled.Detail})");
                    return OutputShutdownOutcome.FinishedWithFailure;
            }

            if (!string.Equals(settled.Detail, lastDetail, StringComparison.Ordinal))
            {
                _log($"shutdown: waiting for outputs to finish ({settled.Detail})");
                lastDetail = settled.Detail;
            }

            var now = _elapsed();
            if (now >= deadline)
            {
                _log($"shutdown: outputs did not finish within {_timeout.TotalSeconds:0}s — closing anyway (last state: {settled.Detail})");
                return OutputShutdownOutcome.TimedOut;
            }

            if (settled.StopNotYetLanded && now - lastStopSentAt >= StopResendInterval)
            {
                var current = CloseGuardPolicy.Evaluate(_readState());
                SendStops(current.RecordingActive, current.StreamingActive, "re-send stop");
                lastStopSentAt = _elapsed();
            }

            var remaining = deadline - _elapsed();
            await _delay(remaining < PollInterval && remaining > TimeSpan.Zero ? remaining : PollInterval).ConfigureAwait(true);
        }
    }

    // Starts the stops and does NOT await them. The transport's stop can hold its command for a
    // long time (a busy core retries its sync for up to 30 s), and only the core's lifecycle is
    // evidence anyway, so the wait loop above is the one bounded place that waits. The synchronous
    // part of each command (the desired-state write) runs right here, on the caller's thread.
    // Each stop is independent: a throwing recording stop must not skip the stream stop.
    private void SendStops(bool recording, bool streaming, string reason)
    {
        if (recording) StartStop(_stopRecording, $"recording {reason}");
        if (streaming) StartStop(_stopStreaming, $"stream {reason}");
    }

    private void StartStop(Func<Task> stop, string what)
    {
        try
        {
            var task = stop();
            _ = task.ContinueWith(
                completed => _log($"shutdown: {what} failed ({completed.Exception?.GetBaseException().Message})"),
                CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }
        catch (Exception ex)
        {
            _log($"shutdown: {what} failed ({ex.GetType().Name}: {ex.Message})");
        }
    }

    private void TryReportProgress(string status)
    {
        try { _reportProgress(status); }
        catch (Exception ex) { _log($"shutdown: progress status failed ({ex.GetType().Name}: {ex.Message})"); }
    }
}
