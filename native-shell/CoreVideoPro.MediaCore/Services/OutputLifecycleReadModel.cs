using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Observed output activity is independent of operator intent.</summary>
public static class OutputLifecycleReadModel
{
    /// <summary>
    /// PR22 vocabulary: requested -&gt; preparing -&gt; producing -&gt; stopping -&gt;
    /// finalizing -&gt; completed/failed/interrupted. `producing` is only reported by
    /// the core while fresh writer progress exists, so it is real evidence of output
    /// rather than a latch set by an accepted command. `live`/`starting` are the
    /// retired names, still read so a newer shell can talk to an older core.
    /// </summary>
    public static bool IsProducing(string? state) => state is "producing" or "live";

    private static bool IsPreparing(string? state) => state is "requested" or "preparing" or "starting";

    public static bool IsRecordingLive(NativeMediaCoreRecordingSession? recording) =>
        recording?.Lifecycle is { } lifecycle
            ? !string.IsNullOrWhiteSpace(lifecycle.SessionId) && IsProducing(lifecycle.State) &&
              lifecycle.Health is "healthy" or "degraded"
            : recording?.Active == true && recording.Status is "recording" or "warning";

    public static string RecordingStatus(NativeMediaCoreRecordingSession? recording)
    {
        var state = recording?.Lifecycle?.State;
        if (state is null) return recording?.Status ?? "idle";
        if (IsPreparing(state)) return "Recording starting — waiting for media";
        if (IsProducing(state)) return IsRecordingLive(recording) ? "Recording" : "Recording activity unverified";
        return state switch
        {
            // Stop reports that stopping has BEGUN. It never claims the file is done.
            "stopping" or "finalizing" => "Recording finalizing — file is not ready yet",
            "completed" => recording!.Lifecycle!.Finalized ? "Recording finalized" : "Recording completion unverified",
            "failed" => "Recording failed",
            // The writer stopped producing and nobody asked it to.
            "interrupted" => "Recording interrupted — the writer stopped making progress",
            "idle" => "Recording idle",
            _ => "Recording status unknown"
        };
    }

    /// <summary>Operator-readable state for one streaming destination (RTMP/SRT/NDI).</summary>
    public static string SenderStatus(NativeMediaCoreOutputSender? sender)
    {
        var state = sender?.Lifecycle?.State;
        if (state is null) return sender?.Status ?? "idle";
        if (IsPreparing(state)) return "Connecting — no media accepted yet";
        if (IsProducing(state)) return sender!.Lifecycle!.Health == "degraded" ? "Streaming (degraded)" : "Streaming";
        return state switch
        {
            "stopping" or "finalizing" => "Stopping",
            "completed" => sender!.Lifecycle!.Finalized ? "Stream ended" : "Stream ended without sending media",
            "failed" => "Stream failed",
            "interrupted" => "Stream interrupted — no media accepted recently",
            "idle" => "idle",
            _ => "Stream status unknown"
        };
    }
}
