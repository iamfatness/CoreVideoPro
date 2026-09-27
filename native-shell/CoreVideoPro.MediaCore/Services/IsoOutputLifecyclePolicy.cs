using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Derives ISO chips from writer observations, never from an ISO selection checkbox.</summary>
public static class IsoOutputLifecyclePolicy
{
    public static IReadOnlyDictionary<string, (string SessionId, string State)> Observe(
        NativeMediaCoreRecordingSession? recording)
    {
        var result = new Dictionary<string, (string, string)>(StringComparer.Ordinal);
        if (recording is null) return result;
        var lifecycle = recording.Lifecycle;
        foreach (var stream in recording.Streams)
        {
            if (stream.Kind != "iso" || string.IsNullOrWhiteSpace(stream.SourceId)) continue;
            var state = lifecycle?.State;
            var error = state is "failed" or "interrupted" || lifecycle?.Health == "failed" ||
                stream.Status is "warning" or "failed" or "error";
            var armed = state is "requested" or "preparing" or "starting";
            var live = lifecycle is not null
                ? state is "producing" or "live" && lifecycle.Health is "healthy" or "degraded"
                : recording.Active && recording.Status is "recording" or "warning";
            var chip = error ? "error" : armed ? "armed" : live ? "recording" : "off";
            if (chip != "off") result[stream.SourceId] = (recording.SessionId, chip);
        }
        return result;
    }
}
