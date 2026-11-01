using System.Globalization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// One operator-facing line for the condition the live show of 2026-10-31 produced (#1074): the
/// stream is in backpressure AND two or more live feeds are stalled at the same instant. Together
/// they point at the operator's network link, not at participants leaving. Every number comes from
/// the core's snapshot (<c>outputSenderSession.senders[].backpressure</c> and the source bus's
/// <c>sources[]</c> health); nothing is measured or guessed here.
/// </summary>
public static class NetworkCongestionReadout
{
    /// <summary>Fewer stalled feeds than this is one source's problem, not the link's.</summary>
    public const int MinStalledFeeds = 2;

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

        return $"network congested: {DescribeStream(pressure)}, {stalled} feeds stalled";
    }

    /// <summary>Live video feeds the source bus currently reports <c>stalled</c>. Zoom and capture
    /// only: a paused clip or a still legitimately re-serves one frame and reads stalled on the
    /// bus's 200 ms diagnostic while perfectly healthy.</summary>
    public static int StalledFeedCount(IReadOnlyList<NativeMediaCoreSourceHealth> sources) =>
        sources.Count(source => source.HasVideo &&
            source.Health == "stalled" &&
            source.Kind is "zoom" or "capture");

    /// <summary>Says what the stream is doing in the words the snapshot can back. Since #538 the
    /// compositor keeps full export cadence while a destination recovers its own queue, so
    /// <c>appliedDivisor</c> is usually 1 under pressure; the queue depth or the pressure level is
    /// then the truthful fact, never a rate the stream is not actually running at.</summary>
    private static string DescribeStream(NativeMediaCoreSenderBackpressure pressure)
    {
        if (pressure.AppliedDivisor > 1)
        {
            return $"stream at 1/{pressure.AppliedDivisor.ToString(CultureInfo.InvariantCulture)} rate";
        }

        if (pressure.BufferedMs > 0)
        {
            return $"stream {Math.Round(pressure.BufferedMs).ToString("0", CultureInfo.InvariantCulture)} ms behind";
        }

        return $"stream backpressure level {pressure.Level.ToString(CultureInfo.InvariantCulture)}";
    }
}
