using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class AudioObservationPolicy
{
    public static double AgeMs(NativeMediaCoreStateSnapshot snapshot, DateTimeOffset now) =>
        Math.Max(0, snapshot.AudioObservation?.AgeMs ?? 0) +
        (snapshot.RawReceivedUtc is { } received ? Math.Max(0, (now - received).TotalMilliseconds) : 0);

    public static string State(NativeMediaCoreStateSnapshot? snapshot, DateTimeOffset now)
    {
        var observation = snapshot?.AudioObservation;
        if (observation is null || observation.Status == "unobserved" || observation.Sequence == 0)
            return "unobserved";
        var age = AgeMs(snapshot!, now);
        return age <= 250 ? "fresh" : age <= 1000 ? "aging" : "stale";
    }

    public static string? MeterWarning(NativeMediaCoreStateSnapshot? snapshot, DateTimeOffset? now = null)
    {
        // Legacy cores lack the envelope; diagnostics reports this as unobserved.
        if (snapshot?.AudioObservation is null) return null;
        return State(snapshot, now ?? DateTimeOffset.UtcNow) switch
        {
            "unobserved" => "awaiting measured audio",
            "aging" => "meters aging",
            "stale" => "meters stale — last values held",
            _ => null
        };
    }

    public static string Describe(NativeMediaCoreStateSnapshot? snapshot, DateTimeOffset? now = null)
    {
        var at = now ?? DateTimeOffset.UtcNow;
        if (State(snapshot, at) == "unobserved") return "Audio telemetry unobserved";
        return $"Audio telemetry {State(snapshot, at)} · {AgeMs(snapshot!, at):0} ms old · " +
               $"revision {snapshot!.AudioObservation!.ObservedRevision}";
    }
}
