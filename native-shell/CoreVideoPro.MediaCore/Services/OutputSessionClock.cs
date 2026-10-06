using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Elapsed local output-session time, not receiver playback duration.
/// Starts at the first observed production. Retry/recovery remains part of the
/// same session until all recording/streaming lifecycles end. Time spent in a
/// retry is elapsed session time, not evidence of delivered media.</summary>
public sealed class OutputSessionClock(TimeProvider? timeProvider = null)
{
    private readonly TimeProvider _time = timeProvider ?? TimeProvider.System;
    private long? _started;

    public double Observe(NativeMediaCoreStateSnapshot snapshot)
    {
        var recording = snapshot.Recording;
        var requested = recording?.Lifecycle is { } lifecycle
            ? IsOpen(lifecycle.State)
            : recording?.Active == true;
        var producing = OutputLifecycleReadModel.IsRecordingLive(recording);
        foreach (var sender in snapshot.OutputSenderSession.Senders)
        {
            requested |= sender.Lifecycle is { } senderLifecycle
                ? IsOpen(senderLifecycle.State)
                : sender.Status is "starting" or "live" or "warning" or "retrying";
            producing |= sender.Lifecycle is { } observed
                ? !string.IsNullOrWhiteSpace(observed.SessionId) &&
                  OutputLifecycleReadModel.IsProducing(observed.State) &&
                  observed.Health is "healthy" or "degraded"
                : sender.Status is "live" or "warning" && sender.FramesSent > 0;
        }
        return Observe(requested, producing);
    }

    private static bool IsOpen(string state) => state is
        "requested" or "preparing" or "starting" or "producing" or "live" or
        "interrupted" or "stopping" or "finalizing";

    public double Observe(bool outputRequested, bool producing)
    {
        if (!outputRequested)
        {
            Reset();
            return 0;
        }
        if (_started is null && producing) _started = _time.GetTimestamp();
        return _started is { } started
            ? Math.Max(0, _time.GetElapsedTime(started).TotalSeconds)
            : 0;
    }

    public void Reset() => _started = null;
}
