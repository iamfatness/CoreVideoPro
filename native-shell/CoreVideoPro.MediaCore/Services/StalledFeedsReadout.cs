using System.Globalization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// One operator-facing line for the condition the live show of 2026-10-31 produced (#1074): the
/// stream is in backpressure AND two or more Zoom feeds are stalled at the same moment. The line
/// states those two facts ("stream 445 ms behind and 3 Zoom feeds stalled") and nothing more: it
/// does not claim a cause. On that show the cause was the operator's uplink, but the snapshot can
/// only see the two symptoms. Every number comes from the core's snapshot
/// (<c>outputSenderSession.senders[].backpressure</c> and the source bus's <c>sources[]</c>);
/// nothing is measured or guessed here.
/// </summary>
public static class StalledFeedsReadout
{
    /// <summary>Fewer stalled feeds than this is one guest's problem, not a shared one.</summary>
    public const int MinStalledFeeds = 2;

    /// <summary>The ON-AIR stall threshold, mirroring the core's <c>kOnAirStallNs</c> (1.5 s,
    /// SourceBus.h): the same age at which a Zoom tile reads "stalled" on air. The bus's own
    /// <c>health</c> turns "stalled" at 200 ms, a diagnostic that flickers on ordinary cadence
    /// noise, so it is never used alone.</summary>
    public const double OnAirStallMs = 1500;

    public static string? Format(NativeMediaCoreStateSnapshot snapshot)
    {
        var pressured = StreamDegradationReadout.PressuredSenders(snapshot).FirstOrDefault();
        if (pressured?.Backpressure is not { } pressure)
        {
            return null;
        }

        var stalled = StalledFeedCount(snapshot.Sources);
        if (stalled < MinStalledFeeds)
        {
            return null;
        }

        return $"{DescribeStream(pressure)} and {stalled.ToString(CultureInfo.InvariantCulture)} Zoom feeds stalled";
    }

    /// <summary>Zoom video sources stalled by the on-air rule: bus health "stalled" AND at least
    /// <see cref="OnAirStallMs"/> since the last new frame. Zoom only: a capture source (a static
    /// screen share, an idle webcam scene) legitimately re-serves one frame for minutes while
    /// healthy, and stills, clips and the Tiles wall itself do the same. A row without
    /// <c>sinceLastNewFrameMs</c> (an older core) is never counted.</summary>
    public static int StalledFeedCount(IReadOnlyList<NativeMediaCoreSourceHealth> sources) =>
        sources.Count(source => source.Kind == "zoom" &&
            source.HasVideo &&
            source.Health == "stalled" &&
            source.SinceLastNewFrameMs is { } age && age >= OnAirStallMs);

    /// <summary>Says what the stream is doing in the words the snapshot can back. Since #538 the
    /// compositor keeps full export cadence while a destination recovers its own queue, so
    /// <c>appliedDivisor</c> is usually 1 under pressure; the queue depth or the pressure level is
    /// then the truthful fact, never a rate the stream is not actually running at.</summary>
    private static string DescribeStream(NativeMediaCoreSenderBackpressure pressure)
    {
        if (pressure.AppliedDivisor > 1)
        {
            return $"Stream at 1/{pressure.AppliedDivisor.ToString(CultureInfo.InvariantCulture)} rate";
        }

        if (pressure.BufferedMs > 0)
        {
            return $"Stream {Math.Round(pressure.BufferedMs).ToString("0", CultureInfo.InvariantCulture)} ms behind";
        }

        return $"Stream backpressure level {pressure.Level.ToString(CultureInfo.InvariantCulture)}";
    }
}
