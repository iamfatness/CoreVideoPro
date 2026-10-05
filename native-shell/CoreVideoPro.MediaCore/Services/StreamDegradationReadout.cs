using System.Globalization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Applied encoder feed cadence is distinct from receiver delivery FPS.</summary>
public static class StreamDegradationReadout
{
    public static string? Format(NativeMediaCoreStateSnapshot snapshot)
    {
        var destinations = snapshot.OutputSenderSession.Senders
            .Where(sender => sender.Status is "live" or "warning" &&
                (sender.Lifecycle is null || sender.Lifecycle.State == "producing") &&
                sender.Backpressure is { } pressure &&
                (pressure.AppliedDivisor > 1 || pressure.Level > 0))
            .Select(sender =>
            {
                var fps = snapshot.OutputProfile.Fps;
                var cadence = fps > 0 && sender.Backpressure!.AppliedDivisor > 0
                    ? ((double)fps / sender.Backpressure!.AppliedDivisor).ToString("0.##", CultureInfo.InvariantCulture) + " fps feed"
                    : "feed rate unknown";
                return $"{sender.Destination.ToUpperInvariant()} degraded · {cadence}";
            }).ToArray();
        return destinations.Length == 0 ? null : string.Join("; ", destinations);
    }
}
