namespace CoreVideoPro.MediaCore.Services;

/// <summary>How the supervisor's crash recovery ended.</summary>
public enum MediaCoreRecoveryOutcome
{
    /// <summary>The core is back and there was no meeting to rejoin.</summary>
    CoreOnly,
    /// <summary>The core is back and Zoom was rejoined.</summary>
    ZoomRejoined,
    /// <summary>The core is back, but the Zoom rejoin failed.</summary>
    ZoomRejoinFailed
}

public enum OutputResumeDecision
{
    /// <summary>The output was not running when the core died, or the operator has since decided.</summary>
    None,
    /// <summary>Start the output again now.</summary>
    Resume,
    /// <summary>The output was lost and cannot be resumed; say why.</summary>
    NotResumed
}

/// <summary>
/// #732: a core that dies mid-show used to end the recording and the stream for good. The file
/// written before the crash survives (measured: 25.7 s recorded, 26.0 s playable), but the
/// supervisor restarted the core and nothing restarted the outputs, so the rest of the show was
/// neither recorded nor streamed. Owner rulings: resume the recording automatically into a new
/// session (2026-10-01) and reconnect the stream (2026-10-02), and say so.
///
/// One instance per output. It holds the one fact that has to outlive the crash: this output
/// was running when the core died. The shell clears its Recording and Streaming intents the
/// moment the core starts recovering (the old session's continuity is gone and must not be
/// re-asserted against a core that has not rejoined Zoom), so without this nobody remembers
/// there was anything to resume.
///
/// Pure and single-threaded by contract: every call is made on the UI thread.
/// </summary>
public sealed class OutputResumeAfterCoreRestart
{
    private readonly bool _requiresZoomRejoin;
    private bool _owed;

    /// <param name="requiresZoomRejoin">
    /// True for the recording: when a meeting was live and Zoom did not come back, the
    /// operator decides, rather than a new session recording slates where the guests were.
    /// A recording made with no meeting (#1009: local sources only) resumes like any other.
    /// False for the stream: a destination that stays disconnected ends the broadcast, so
    /// Program goes back out whatever happened to Zoom.
    /// </param>
    public OutputResumeAfterCoreRestart(bool requiresZoomRejoin) => _requiresZoomRejoin = requiresZoomRejoin;

    /// <summary>True while an output lost to a crash has not been resumed or given up.</summary>
    public bool Owed => _owed;

    /// <summary>The core died. <paramref name="outputWasOn"/> is the operator's intent at that
    /// moment. A second crash before the resume arrives with the intent already cleared; it
    /// must not forget the first.</summary>
    public void CoreCrashed(bool outputWasOn)
    {
        if (outputWasOn) _owed = true;
    }

    /// <summary>The operator pressed the button (start or stop) themselves. Their choice
    /// stands; nothing is started behind it afterwards.</summary>
    public void OperatorChose() => _owed = false;

    /// <summary>The app is stopping the core on purpose (leave, engine stop, exit).</summary>
    public void CoreStoppedDeliberately() => _owed = false;

    /// <summary>The supervisor finished recovering. Decides once; a later call returns None.</summary>
    public OutputResumeDecision RecoveryCompleted(MediaCoreRecoveryOutcome outcome)
    {
        if (!_owed) return OutputResumeDecision.None;
        _owed = false;
        return !_requiresZoomRejoin || outcome != MediaCoreRecoveryOutcome.ZoomRejoinFailed
            ? OutputResumeDecision.Resume
            : OutputResumeDecision.NotResumed;
    }

    public static string DescribeRecordingNotResumed(MediaCoreRecoveryOutcome outcome) => outcome switch
    {
        MediaCoreRecoveryOutcome.ZoomRejoinFailed =>
            "Recording stopped when the media core restarted and was NOT resumed: Zoom did not rejoin. Rejoin the meeting, then press Record.",
        _ =>
            "Recording stopped when the media core restarted and was NOT resumed. Press Record when ready."
    };
}
