using System.Diagnostics;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

internal enum OutputShutdownOutcome
{
    /// <summary>Nothing was recording or streaming, at the close request or now: nothing was sent.</summary>
    NothingLive,

    /// <summary>The core reported every baseline destination completed / idle, for this stop.</summary>
    Finished,

    /// <summary>Everything stopped, but a baseline destination ended failed or interrupted.</summary>
    FinishedWithFailure,

    /// <summary>The core is not running, so there is nothing left that can be saved.</summary>
    CoreUnavailable,

    /// <summary>The core died and was respawned during the wait: the files it was writing were
    /// interrupted with it, and the new core's state says nothing about them.</summary>
    CoreRestarted,

    /// <summary>The outputs did not finish within <see cref="OutputShutdownCoordinator.FinishTimeout"/>.</summary>
    TimedOut
}

/// <summary>
/// "Stop and close" (T1.8, #461): stop recording and streaming through the EXISTING transport
/// commands, then wait for the core to say THOSE files are finished before the app shuts the core
/// down. Bounded by <see cref="FinishTimeout"/>; on timeout it logs loudly and lets the close
/// proceed (never a hang).
///
/// <para><b>Evidence, and why each rule exists (fix round 1).</b></para>
/// <list type="bullet">
/// <item><b>Freshness by <c>RawReceivedUtc</c>.</b> Only a snapshot the shell RECEIVED after the
/// last moment the stop was still pending (intent set, or a Record/Stream command in flight) is
/// evidence. A snapshot that left the core before the stop landed can carry a stale
/// <c>completed</c>. The coordinator reads the bridge's <c>LastSnapshot</c> (refreshed every
/// 250 ms by its poll) and never issues its own syncs, so it cannot compete with the stop for the
/// supervisor's single sync slot.</item>
/// <item><b>Session binding.</b> <see cref="CloseGuardPolicy.EvaluateSettled"/> accepts a
/// terminal state only for the recording session / senders in the <see cref="OutputStopBaseline"/>
/// captured at the close request; an absent node is pending.</item>
/// <item><b>Core generation.</b> A changed restart count means a respawned core answered; its
/// <c>idle</c> is not the old file's completion, so the wait ends at once as
/// <see cref="OutputShutdownOutcome.CoreRestarted"/>, logged as an interrupted recording. A core
/// that is simply gone ends it as <see cref="OutputShutdownOutcome.CoreUnavailable"/>: nothing more
/// can be saved.</item>
/// </list>
///
/// <para><b>Threading.</b> Construct and call it on the UI thread. The transport stop commands write
/// bound properties before their first await, and every await here resumes on the caller's context,
/// so a re-sent stop also runs on the UI thread (the 0xc000027b rule). Every dependency is injected
/// so the whole wait is testable without a core or a window.</para>
/// </summary>
internal sealed class OutputShutdownCoordinator
{
    /// <summary>The longest the app waits for recording / streams to finish before closing anyway.</summary>
    internal static readonly TimeSpan FinishTimeout = TimeSpan.FromSeconds(15);

    /// <summary>How often the wait re-reads the bridge's latest snapshot (the bridge polls at 250 ms).</summary>
    internal static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(250);

    // A stop that has not landed is re-sent at most this often. The transport's own in-flight
    // guards keep re-sends from piling up.
    internal static readonly TimeSpan StopResendInterval = TimeSpan.FromSeconds(1);

    internal const string ProgressFinishingRecording = "Finishing the recording before closing…";
    internal const string ProgressStoppingStreams = "Stopping streams before closing…";

    private readonly Func<CloseGuardInput> _readState;
    private readonly Func<Task> _stopRecording;
    private readonly Func<Task> _stopStreaming;
    private readonly Action<string> _reportProgress;
    private readonly Action<string> _log;
    private readonly Func<TimeSpan> _elapsed;
    private readonly Func<DateTimeOffset> _utcNow;
    private readonly Func<TimeSpan, Task> _delay;
    private readonly TimeSpan _timeout;

    /// <param name="readState">The shell's flags, in-flight guards, core generation and the bridge's
    /// latest snapshot. Read once per tick; never triggers a core round trip.</param>
    /// <param name="stopRecording">The existing transport stop (SetRecordingAsync(false)).</param>
    /// <param name="stopStreaming">The existing transport stop (SetStreamingAsync(false)).</param>
    /// <param name="reportProgress">Writes the operator-visible status line.</param>
    /// <param name="log">The launch log.</param>
    /// <param name="elapsed">Monotonic time since construction; defaults to a Stopwatch.</param>
    /// <param name="utcNow">The clock <c>RawReceivedUtc</c> is compared against; defaults to UtcNow.</param>
    /// <param name="delay">Waits between ticks; defaults to Task.Delay.</param>
    /// <param name="timeout">Defaults to <see cref="FinishTimeout"/>.</param>
    internal OutputShutdownCoordinator(
        Func<CloseGuardInput> readState,
        Func<Task> stopRecording,
        Func<Task> stopStreaming,
        Action<string> reportProgress,
        Action<string> log,
        Func<TimeSpan>? elapsed = null,
        Func<DateTimeOffset>? utcNow = null,
        Func<TimeSpan, Task>? delay = null,
        TimeSpan? timeout = null)
    {
        _readState = readState;
        _stopRecording = stopRecording;
        _stopStreaming = stopStreaming;
        _reportProgress = reportProgress;
        _log = log;
        if (elapsed is null)
        {
            var stopwatch = Stopwatch.StartNew();
            elapsed = () => stopwatch.Elapsed;
        }

        _elapsed = elapsed;
        _utcNow = utcNow ?? (() => DateTimeOffset.UtcNow);
        _delay = delay ?? (interval => Task.Delay(interval));
        _timeout = timeout ?? FinishTimeout;
    }

    private static bool IntentPending(CloseGuardInput state) =>
        state.RecordingRequested || state.StreamingRequested || state.RecordingToggleInFlight || state.StreamToggleInFlight;

    /// <param name="closeRequest">The decision taken when the close was requested; its baseline
    /// names the sessions this stop must see finish. Null: take the baseline now.</param>
    internal async Task<OutputShutdownOutcome> StopAndWaitAsync(CloseGuardDecision? closeRequest = null)
    {
        var atStop = _readState();
        var decisionNow = CloseGuardPolicy.Evaluate(atStop);
        if (!decisionNow.ShouldAsk && closeRequest?.ShouldAsk != true)
        {
            _log("shutdown: no recording or stream is live; nothing to finish before closing");
            return OutputShutdownOutcome.NothingLive;
        }

        var baseline = closeRequest is { ShouldAsk: true }
            ? CloseGuardPolicy.Merge(closeRequest.Baseline, decisionNow.Baseline)
            : decisionNow.Baseline;
        var description = decisionNow.ShouldAsk ? decisionNow.Description : closeRequest!.Description;

        // Fix round 2 (N2): the "same core" baseline is armed when the stop is actually SENT. A
        // core that respawned while the dialog was open took the old files with it (logged), but
        // the new core may already be recording again (the shell re-arms its desired state), and
        // that output must be stopped and waited for too — never left to the exit kill.
        if (atStop.CoreRunning &&
            closeRequest is { ShouldAsk: true } &&
            atStop.CoreGeneration != closeRequest.Baseline.CoreGeneration)
        {
            _log($"shutdown: MEDIA CORE RESTARTED while the close prompt was open (generation {closeRequest.Baseline.CoreGeneration} -> {atStop.CoreGeneration}); " +
                 "the old core's recording was interrupted and nothing more can be saved for it; stopping what the new core is doing");
            if (!decisionNow.ShouldAsk)
            {
                return OutputShutdownOutcome.CoreRestarted;
            }

            baseline = decisionNow.Baseline;
            description = decisionNow.Description;
        }

        if (CoreGone(atStop, baseline) is { } gone)
        {
            return gone;
        }

        var started = _elapsed();
        var deadline = started + _timeout;
        _log($"shutdown: stopping outputs before close ({description}); waiting up to {_timeout.TotalSeconds:0}s for them to finish " +
             $"(evidence: snapshots received after the stop, recording session {baseline.RecordingSessionId ?? "none"}, " +
             $"{baseline.Senders.Count} sender(s), core generation {baseline.CoreGeneration})");
        TryReportProgress(baseline.RecordingExpected ? ProgressFinishingRecording : ProgressStoppingStreams);

        SendStops(decisionNow.RecordingActive, decisionNow.StreamingActive, "stop");
        var lastStopSentAt = _elapsed();
        // Only a snapshot received after this instant can be this stop's evidence. It moves forward
        // every tick the stop is still pending (intent set or a command in flight).
        var evidenceFloor = _utcNow();

        var lastDetail = string.Empty;
        while (true)
        {
            var state = _readState();
            if (CoreGone(state, baseline) is { } goneNow)
            {
                return goneNow;
            }

            if (IntentPending(state))
            {
                evidenceFloor = _utcNow();
            }

            var snapshot = state.Snapshot is { RawReceivedUtc: { } received } candidate && received > evidenceFloor
                ? candidate
                : null;
            var settled = CloseGuardPolicy.EvaluateSettled(snapshot, baseline, state);
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
                var current = CloseGuardPolicy.Evaluate(state);
                SendStops(current.RecordingActive || state.RecordingRequested, current.StreamingActive || state.StreamingRequested, "re-send stop");
                lastStopSentAt = _elapsed();
                evidenceFloor = _utcNow();
            }

            var remaining = deadline - _elapsed();
            await _delay(remaining < PollInterval && remaining > TimeSpan.Zero ? remaining : PollInterval).ConfigureAwait(true);
        }
    }

    private OutputShutdownOutcome? CoreGone(CloseGuardInput state, OutputStopBaseline baseline)
    {
        if (state.CoreGeneration != baseline.CoreGeneration)
        {
            _log($"shutdown: MEDIA CORE RESTARTED while outputs were finishing (generation {baseline.CoreGeneration} -> {state.CoreGeneration}); " +
                 "the recording was interrupted with the old core and nothing more can be saved; closing");
            return OutputShutdownOutcome.CoreRestarted;
        }

        if (!state.CoreRunning)
        {
            _log("shutdown: MEDIA CORE IS NOT RUNNING while outputs were finishing; nothing more can be saved; closing");
            return OutputShutdownOutcome.CoreUnavailable;
        }

        return null;
    }

    // Starts the stops and does NOT await them. The transport's stop can hold its command for a
    // long time (a busy core retries its sync for up to 30 s), and only the core's lifecycle is
    // evidence anyway, so the wait loop above is the one bounded place that waits. The synchronous
    // part of each command (the desired-state write) runs right here, on the caller's thread.
    // Each stop is independent: a throwing stop must not skip the other one.
    //
    // ORDER MATTERS — STREAMS FIRST, THEN RECORDING (fix round 2, N1). Each transport stop builds
    // its sync payload synchronously, inline, from the CURRENT desired flags. If the recording
    // stop ran first, its batch would carry Recording=false while Streaming was still true:
    // `stop-recording-session` then `start-program-output{rtmp…}`. The core's startProgramOutput
    // no longer sees the encoder as owned by a recording (`recordingStatus_` is "stopping"), so it
    // calls encoder->start([rtmp…]); that bumps the sink generation and resets its snapshot with
    // NO recording lifecycle, and the old-generation stop barrier finalizes the file but never
    // publishes `completed` (AsyncEncoderSink.cpp:117-132, :659; MediaCore.cpp:2363-2370). The
    // shell then waits the full 15 s for evidence that can never arrive. Stopping the streams
    // first means the stream stop's batch still carries Recording=true (no encoder restart), and
    // the recording stop's batch carries both flags false (no start-program-output at all). The
    // underlying core defect (a non-recording Start erasing a finalizing recording's lifecycle)
    // is filed separately; this ordering is the shell-side mitigation.
    private void SendStops(bool recording, bool streaming, string reason)
    {
        if (streaming) StartStop(_stopStreaming, $"stream {reason}");
        if (recording) StartStop(_stopRecording, $"recording {reason}");
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
