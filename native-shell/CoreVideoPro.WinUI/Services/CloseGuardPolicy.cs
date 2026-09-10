using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>What the shell knows about its outputs at the moment the operator closes the window.</summary>
/// <param name="CoreRunning">The media core process is up. With no core there is nothing left to save.</param>
/// <param name="RecordingRequested">Operator intent (the transport's desired Recording flag).</param>
/// <param name="RecordingObserved">The shell's live Recording indicator (observed media).</param>
/// <param name="StreamingRequested">Operator intent (the transport's desired Streaming flag).</param>
/// <param name="StreamingObserved">The shell's live Streaming indicator (observed media).</param>
/// <param name="Snapshot">The latest core snapshot (recording lifecycle + each sender's lifecycle), or null.</param>
internal sealed record CloseGuardInput(
    bool CoreRunning,
    bool RecordingRequested,
    bool RecordingObserved,
    bool StreamingRequested,
    bool StreamingObserved,
    NativeMediaCoreStateSnapshot? Snapshot);

/// <summary>Whether closing needs to ask first, and a short plain-English line saying what is live.</summary>
internal sealed record CloseGuardDecision(
    bool ShouldAsk,
    string Description,
    bool RecordingActive,
    bool StreamingActive,
    int LiveStreamDestinations)
{
    internal static CloseGuardDecision Nothing { get; } = new(false, string.Empty, false, false, 0);
}

/// <summary>Where the outputs stand while the app waits to close (see <see cref="CloseGuardPolicy.EvaluateSettled"/>).</summary>
internal enum OutputSettleState
{
    /// <summary>Recording and every sender reached completed / idle (or were never there).</summary>
    Settled,

    /// <summary>Everything stopped, but at least one destination ended failed or interrupted.</summary>
    SettledWithFailure,

    /// <summary>Something is still live, stopping or finalizing — or there is no evidence yet.</summary>
    Pending
}

/// <param name="StopNotYetLanded">A destination is still in its active states (requested ..
/// producing), i.e. the stop has not reached it yet, so the stop request should be re-sent.</param>
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
/// it has written the file's index. Either source saying "live" is enough — the shell's flags OR the
/// core's lifecycle — because asking once too often costs one click and not asking costs the show.
/// A destination with no lifecycle (an older core) is judged by the shell flag, never assumed
/// healthy or idle. The virtual camera alone never asks: closing it corrupts nothing.</para>
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

    internal static CloseGuardDecision Evaluate(CloseGuardInput input)
    {
        if (!input.CoreRunning)
        {
            return CloseGuardDecision.Nothing;
        }

        var recordingState = input.Snapshot?.Recording?.Lifecycle?.State;
        var recordingFlag = input.RecordingRequested || input.RecordingObserved;
        var recordingActive = recordingFlag || IsLiveState(recordingState);
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
            ({ } recording, { } streaming) => $"{recording} and {LowerFirst(streaming)}",
            ({ } recording, null) => recording,
            (null, { } streaming) => streaming,
            _ => string.Empty
        };

        return new CloseGuardDecision(true, description, recordingActive, streamingActive, liveDestinations);
    }

    /// <summary>
    /// After Stop has been sent: have the outputs actually finished? Evidence only — a snapshot
    /// from the core. No snapshot is not evidence (Pending). A recording with no lifecycle (older
    /// core) is finished when the core reports it inactive.
    /// </summary>
    internal static OutputSettleResult EvaluateSettled(NativeMediaCoreStateSnapshot? snapshot)
    {
        if (snapshot is null)
        {
            return new OutputSettleResult(OutputSettleState.Pending, false, "no core snapshot yet");
        }

        var pending = new List<string>();
        var stopNotLanded = false;
        var failed = new List<string>();

        var recording = snapshot.Recording;
        if (recording?.Lifecycle is { } recordingLifecycle)
        {
            var state = recordingLifecycle.State;
            if (!IsSettledState(state))
            {
                pending.Add($"recording {state}");
                stopNotLanded |= IsRunningState(state);
            }
            else if (IsFailureState(state))
            {
                failed.Add($"recording {state}");
            }
        }
        else if (recording?.Active == true)
        {
            pending.Add($"recording {recording.Status}");
        }

        foreach (var sender in snapshot.OutputSenderSession.Senders)
        {
            if (sender.Lifecycle is { } senderLifecycle)
            {
                var state = senderLifecycle.State;
                if (!IsSettledState(state))
                {
                    pending.Add($"{sender.Destination} {state}");
                    stopNotLanded |= IsRunningState(state);
                }
                else if (IsFailureState(state))
                {
                    failed.Add($"{sender.Destination} {state}");
                }
            }
            else if (IsLegacySenderActive(sender))
            {
                pending.Add($"{sender.Destination} {sender.Status}");
                stopNotLanded = true;
            }
        }

        if (pending.Count > 0)
        {
            return new OutputSettleResult(OutputSettleState.Pending, stopNotLanded, string.Join(", ", pending));
        }

        return failed.Count > 0
            ? new OutputSettleResult(OutputSettleState.SettledWithFailure, false, string.Join(", ", failed))
            : new OutputSettleResult(OutputSettleState.Settled, false, "all outputs stopped");
    }

    private static string LowerFirst(string text) =>
        text.Length == 0 ? text : char.ToLowerInvariant(text[0]) + text[1..];
}
