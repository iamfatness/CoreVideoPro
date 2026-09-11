using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>What the shell knows about its outputs at one moment (a close request, or one tick of
/// the finishing wait).</summary>
/// <param name="CoreRunning">The media core process is up. With no core there is nothing left to save.</param>
/// <param name="RecordingRequested">Operator intent (the transport's desired Recording flag).</param>
/// <param name="RecordingObserved">The shell's live Recording indicator (observed media).</param>
/// <param name="StreamingRequested">Operator intent (the transport's desired Streaming flag).</param>
/// <param name="StreamingObserved">The shell's live Streaming indicator (observed media).</param>
/// <param name="Snapshot">The bridge's latest core snapshot, or null.</param>
/// <param name="RecordingToggleInFlight">The transport's Record command is still running (a start
/// or stop that has not reached the core yet — while it runs, a new stop is silently ignored).</param>
/// <param name="StreamToggleInFlight">Same for the Stream command.</param>
/// <param name="CoreGeneration">Which core process answered: the supervisor's restart count.
/// A change means the core died and was respawned, and every file it was writing is gone.</param>
internal sealed record CloseGuardInput(
    bool CoreRunning,
    bool RecordingRequested,
    bool RecordingObserved,
    bool StreamingRequested,
    bool StreamingObserved,
    NativeMediaCoreStateSnapshot? Snapshot,
    bool RecordingToggleInFlight = false,
    bool StreamToggleInFlight = false,
    long CoreGeneration = 0);

/// <summary>
/// What was live when the close was requested — the destinations a "Stop and close" must see
/// finish. Evidence is only accepted for THESE: a terminal state belonging to another recording
/// session, an older failure, or another core generation is not this stop's evidence.
/// </summary>
/// <param name="CoreGeneration">The core generation that owns the files being finished.</param>
/// <param name="RecordingSessionId">The recording lifecycle session that was live (non-terminal),
/// or null when recording was live only by the shell flag (start still in flight) or on a core
/// with no lifecycle.</param>
/// <param name="StaleRecordingSessionId">A terminal recording session already in the snapshot at
/// close time. Its <c>completed</c>/<c>failed</c> is history, never this stop's result.</param>
/// <param name="LegacyRecording">Recording was live per <c>recording.active</c> on a core that
/// publishes no lifecycle.</param>
/// <param name="Senders">Sender id → the sender lifecycle session id (null: no lifecycle) for every
/// sender that was live.</param>
internal sealed record OutputStopBaseline(
    long CoreGeneration,
    bool RecordingExpected,
    string? RecordingSessionId,
    string? StaleRecordingSessionId,
    bool LegacyRecording,
    bool StreamingExpected,
    IReadOnlyDictionary<string, string?> Senders)
{
    internal static OutputStopBaseline None { get; } =
        new(0, false, null, null, false, false, new Dictionary<string, string?>());
}

/// <summary>Whether closing needs to ask first, a short plain-English line saying what is live,
/// and the baseline a later "Stop and close" must see finish.</summary>
internal sealed record CloseGuardDecision(
    bool ShouldAsk,
    string Description,
    bool RecordingActive,
    bool StreamingActive,
    int LiveStreamDestinations,
    OutputStopBaseline Baseline)
{
    internal static CloseGuardDecision Nothing { get; } =
        new(false, string.Empty, false, false, 0, OutputStopBaseline.None);
}

/// <summary>Where the outputs stand while the app waits to close (see <see cref="CloseGuardPolicy.EvaluateSettled"/>).</summary>
internal enum OutputSettleState
{
    /// <summary>Every baseline destination reached completed / idle, and nothing new is live.</summary>
    Settled,

    /// <summary>Everything stopped, but a BASELINE destination ended failed or interrupted.</summary>
    SettledWithFailure,

    /// <summary>Something is still live, stopping or finalizing — or there is no evidence yet.</summary>
    Pending
}

/// <param name="StopNotYetLanded">The stop has not reached something yet (intent still set, or a
/// destination still requested..producing), so the stop request should be re-sent.</param>
internal sealed record OutputSettleResult(OutputSettleState State, bool StopNotYetLanded, string Detail);

/// <summary>
/// T1.8 (#461): closing the app while recording or streaming used to kill-tree the media core with
/// no stop sent, so the Program MP4's moov atom and every ISO writer were cut off mid-write and the
/// show recording was unplayable. The owner's decision is "ask first". This policy decides WHEN to
/// ask and WHEN the outputs have finished; it is pure (no UI, no bridge) so it is unit-tested.
///
/// <para><b>What counts as live.</b> The destination lifecycle vocabulary is
/// requested → preparing → producing → stopping → finalizing → completed | failed | interrupted
/// (plus idle, and the retired starting / live). Everything before a terminal state counts, and
/// that includes <c>stopping</c> and <c>finalizing</c>: closing then still kills the writer before
/// it has written the file's index. Any source saying "live" is enough — the shell's flags, the
/// core's lifecycle, or (on an older core with no lifecycle) <c>recording.active</c> — because
/// asking once too often costs one click and not asking costs the show. The virtual camera alone
/// never asks: closing it corrupts nothing.</para>
///
/// <para><b>What counts as finished</b> (<see cref="EvaluateSettled"/>, fix round 1): evidence
/// tied to THIS stop. The transport's intent must be clear and no Record/Stream command in flight
/// (a stop sent while a start is in flight is silently ignored). Every destination in the
/// <see cref="OutputStopBaseline"/> must show a terminal state for ITS session; an absent node is
/// unknown, never finished; nothing new may be running. The caller supplies only a snapshot the
/// shell received after the last moment intent was still set (freshness, by
/// <c>RawReceivedUtc</c>) and handles a changed core generation itself.</para>
/// </summary>
internal static class CloseGuardPolicy
{
    internal const string DialogTitle = "Stop outputs and close?";
    internal const string StopAndCloseButton = "Stop and close";
    internal const string KeepRunningButton = "Keep running";
    internal const string DialogConsequence = "Closing now would cut the recording off before it's saved.";

    internal static string DialogBody(CloseGuardDecision decision) =>
        $"{decision.Description}. {DialogConsequence}";

    /// <summary>Not yet stopped: requested / preparing / producing, and the retired starting / live.</summary>
    internal static bool IsRunningState(string? state) =>
        state is "requested" or "preparing" or "starting" or "producing" or "live";

    /// <summary>Stop has begun but the file / stream is not finished yet.</summary>
    internal static bool IsStoppingState(string? state) => state is "stopping" or "finalizing";

    internal static bool IsFailureState(string? state) => state is "failed" or "interrupted";

    /// <summary>Terminal or never started: nothing left that closing could corrupt.</summary>
    internal static bool IsSettledState(string? state) =>
        state is "completed" or "failed" or "interrupted" or "idle";

    // An unknown state string is not evidence of anything finished, so it counts as live.
    private static bool IsLiveState(string? state) => state is not null && !IsSettledState(state);

    // A sender without a lifecycle (older core): the three sender statuses that mean media is,
    // or is about to be, going out.
    private static bool IsLegacySenderActive(NativeMediaCoreOutputSender sender) =>
        sender.Status is "live" or "warning" or "starting";

    private static bool IsSenderLive(NativeMediaCoreOutputSender sender) =>
        sender.Lifecycle is { } lifecycle ? IsLiveState(lifecycle.State) : IsLegacySenderActive(sender);

    private static bool IsLegacyRecordingActive(NativeMediaCoreRecordingSession? recording) =>
        recording is { Lifecycle: null, Active: true };

    internal static CloseGuardDecision Evaluate(CloseGuardInput input)
    {
        if (!input.CoreRunning)
        {
            return CloseGuardDecision.Nothing;
        }

        var recording = input.Snapshot?.Recording;
        var recordingState = recording?.Lifecycle?.State;
        var recordingFlag = input.RecordingRequested || input.RecordingObserved;
        var recordingActive = recordingFlag || IsLiveState(recordingState) || IsLegacyRecordingActive(recording);
        var recordingFinalizing = IsStoppingState(recordingState) && !input.RecordingRequested;

        var liveSenderStates = (input.Snapshot?.OutputSenderSession.Senders ?? [])
            .Select(sender => sender.Lifecycle?.State)
            .Where(IsLiveState)
            .ToList();
        var liveDestinations = liveSenderStates.Count;
        var streamingFlag = input.StreamingRequested || input.StreamingObserved;
        var streamingActive = streamingFlag || liveDestinations > 0;
        var streamsStopping = liveDestinations > 0 && !input.StreamingRequested && liveSenderStates.All(IsStoppingState);

        if (!recordingActive && !streamingActive)
        {
            return CloseGuardDecision.Nothing;
        }

        string? recordingPart = recordingActive
            ? recordingFinalizing ? "Recording is still finalizing" : "Recording"
            : null;
        string? streamingPart = !streamingActive
            ? null
            : liveDestinations == 0
                ? "Streaming"
                : streamsStopping
                    ? liveDestinations == 1 ? "A stream is still stopping" : $"{liveDestinations} streams are still stopping"
                    : liveDestinations == 1 ? "Streaming to 1 destination" : $"Streaming to {liveDestinations} destinations";

        var description = (recordingPart, streamingPart) switch
        {
            ({ } recordingText, { } streaming) => $"{recordingText} and {LowerFirst(streaming)}",
            ({ } recordingText, null) => recordingText,
            (null, { } streaming) => streaming,
            _ => string.Empty
        };

        return new CloseGuardDecision(true, description, recordingActive, streamingActive, liveDestinations,
            CaptureBaseline(input, recordingActive, streamingActive));
    }

    private static OutputStopBaseline CaptureBaseline(CloseGuardInput input, bool recordingActive, bool streamingActive)
    {
        var recording = input.Snapshot?.Recording;
        var lifecycle = recording?.Lifecycle;
        var liveSession = lifecycle is not null && IsLiveState(lifecycle.State) ? lifecycle.SessionId : null;
        var staleSession = lifecycle is not null && !IsLiveState(lifecycle.State) ? lifecycle.SessionId : null;
        var senders = new Dictionary<string, string?>(StringComparer.Ordinal);
        foreach (var sender in input.Snapshot?.OutputSenderSession.Senders ?? [])
        {
            if (IsSenderLive(sender))
            {
                senders[SenderKey(sender)] = sender.Lifecycle?.SessionId;
            }
        }

        return new OutputStopBaseline(
            input.CoreGeneration,
            recordingActive,
            liveSession,
            staleSession,
            IsLegacyRecordingActive(recording),
            streamingActive,
            senders);
    }

    /// <summary>
    /// The close-request baseline, widened by what is live at the moment the stop is sent (the
    /// operator may have started something while the dialog was open). The close-request core
    /// generation is kept: if the core restarted in between, those files are already gone.
    /// </summary>
    internal static OutputStopBaseline Merge(OutputStopBaseline atCloseRequest, OutputStopBaseline atStop)
    {
        var senders = new Dictionary<string, string?>(atCloseRequest.Senders, StringComparer.Ordinal);
        foreach (var (id, session) in atStop.Senders)
        {
            senders.TryAdd(id, session);
        }

        return new OutputStopBaseline(
            atCloseRequest.CoreGeneration,
            atCloseRequest.RecordingExpected || atStop.RecordingExpected,
            // The session the stop targets is the one live when it is sent; fall back to the one
            // that was live at the close request.
            atStop.RecordingSessionId ?? atCloseRequest.RecordingSessionId,
            atCloseRequest.StaleRecordingSessionId ?? atStop.StaleRecordingSessionId,
            atCloseRequest.LegacyRecording || atStop.LegacyRecording,
            atCloseRequest.StreamingExpected || atStop.StreamingExpected,
            senders);
    }

    private static string SenderKey(NativeMediaCoreOutputSender sender) =>
        string.IsNullOrEmpty(sender.SenderId) ? sender.Destination : sender.SenderId;

    /// <summary>
    /// After Stop has been sent: have the BASELINE destinations finished, for this stop?
    /// </summary>
    /// <param name="snapshot">A snapshot received after the last moment intent was still set, or
    /// null when none has arrived yet (never evidence).</param>
    /// <param name="baseline">What was live when the close was requested / the stop was sent.</param>
    /// <param name="now">The shell's current flags and in-flight guards.</param>
    internal static OutputSettleResult EvaluateSettled(
        NativeMediaCoreStateSnapshot? snapshot,
        OutputStopBaseline baseline,
        CloseGuardInput now)
    {
        var pending = new List<string>();
        var failed = new List<string>();
        var stopNotLanded = false;

        // Intent first: a stop that has not been accepted by the transport has not been sent.
        if (now.RecordingRequested)
        {
            pending.Add("recording still requested");
            stopNotLanded = true;
        }

        if (now.StreamingRequested)
        {
            pending.Add("streaming still requested");
            stopNotLanded = true;
        }

        if (now.RecordingToggleInFlight) pending.Add("a Record command is still in flight");
        if (now.StreamToggleInFlight) pending.Add("a Stream command is still in flight");

        if (snapshot is null)
        {
            pending.Add("no core snapshot received since the stop was sent");
            return new OutputSettleResult(OutputSettleState.Pending, stopNotLanded, string.Join(", ", pending));
        }

        EvaluateRecording(snapshot.Recording, baseline, pending, failed, ref stopNotLanded);
        EvaluateSenders(snapshot.OutputSenderSession.Senders, baseline, pending, failed, ref stopNotLanded);

        if (pending.Count > 0)
        {
            return new OutputSettleResult(OutputSettleState.Pending, stopNotLanded, string.Join(", ", pending));
        }

        return failed.Count > 0
            ? new OutputSettleResult(OutputSettleState.SettledWithFailure, false, string.Join(", ", failed))
            : new OutputSettleResult(OutputSettleState.Settled, false, "all outputs stopped");
    }

    private static void EvaluateRecording(
        NativeMediaCoreRecordingSession? recording,
        OutputStopBaseline baseline,
        List<string> pending,
        List<string> failed,
        ref bool stopNotLanded)
    {
        var lifecycle = recording?.Lifecycle;
        if (baseline.RecordingSessionId is { } session)
        {
            if (lifecycle is null)
            {
                // Absent lifecycle means UNKNOWN, never finished.
                pending.Add($"no recording evidence for session {session}");
                return;
            }

            if (!string.Equals(lifecycle.SessionId, session, StringComparison.Ordinal))
            {
                pending.Add($"recording reports session {lifecycle.SessionId} ({lifecycle.State}), not the session being stopped");
                stopNotLanded |= IsRunningState(lifecycle.State);
                return;
            }

            if (!IsSettledState(lifecycle.State))
            {
                pending.Add($"recording {lifecycle.State}");
                stopNotLanded |= IsRunningState(lifecycle.State);
            }
            else if (IsFailureState(lifecycle.State))
            {
                failed.Add($"recording {lifecycle.State}");
            }

            return;
        }

        if (baseline.LegacyRecording && lifecycle is null)
        {
            if (recording is null)
            {
                pending.Add("no recording evidence");
            }
            else if (recording.Active)
            {
                pending.Add($"recording {recording.Status}");
            }

            return;
        }

        // No live session at the close request (recording live only by the flag, e.g. a start
        // still in flight). Whatever is running now must finish; a terminal state counts as a
        // failure only for a session that is not the stale one from before the close.
        if (lifecycle is not null)
        {
            if (!IsSettledState(lifecycle.State))
            {
                pending.Add($"recording {lifecycle.State}");
                stopNotLanded |= IsRunningState(lifecycle.State);
            }
            else if (IsFailureState(lifecycle.State) &&
                     baseline.RecordingExpected &&
                     !string.Equals(lifecycle.SessionId, baseline.StaleRecordingSessionId, StringComparison.Ordinal))
            {
                failed.Add($"recording {lifecycle.State}");
            }
        }
        else if (recording?.Active == true)
        {
            pending.Add($"recording {recording.Status}");
        }
    }

    private static void EvaluateSenders(
        IReadOnlyList<NativeMediaCoreOutputSender> senders,
        OutputStopBaseline baseline,
        List<string> pending,
        List<string> failed,
        ref bool stopNotLanded)
    {
        var byKey = new Dictionary<string, NativeMediaCoreOutputSender>(StringComparer.Ordinal);
        foreach (var sender in senders)
        {
            byKey.TryAdd(SenderKey(sender), sender);
        }

        foreach (var (key, _) in baseline.Senders)
        {
            if (!byKey.TryGetValue(key, out var sender))
            {
                pending.Add($"no evidence for {key}");
                continue;
            }

            if (sender.Lifecycle is { } lifecycle)
            {
                if (!IsSettledState(lifecycle.State))
                {
                    pending.Add($"{sender.Destination} {lifecycle.State}");
                    stopNotLanded |= IsRunningState(lifecycle.State);
                }
                else if (IsFailureState(lifecycle.State))
                {
                    failed.Add($"{sender.Destination} {lifecycle.State}");
                }
            }
            else if (IsLegacySenderActive(sender))
            {
                pending.Add($"{sender.Destination} {sender.Status}");
                stopNotLanded = true;
            }
        }

        // A sender that was not live at the close request only matters if it is live now (a
        // stream started during the wait). Its old terminal states are history.
        foreach (var (key, sender) in byKey)
        {
            if (baseline.Senders.ContainsKey(key) || !IsSenderLive(sender))
            {
                continue;
            }

            var state = sender.Lifecycle?.State ?? sender.Status;
            pending.Add($"{sender.Destination} {state}");
            stopNotLanded |= sender.Lifecycle is null || IsRunningState(sender.Lifecycle.State);
        }
    }

    private static string LowerFirst(string text) =>
        text.Length == 0 ? text : char.ToLowerInvariant(text[0]) + text[1..];
}
