namespace CoreVideoPro.WinUI.Services;

internal sealed record MonitorInterval(string Surface, double Seconds, long Frames, double MaximumGapMs);

internal sealed record MonitorPerformanceVerdict(bool FunctionalPassed, bool SubmissionCadencePassed,
    bool FramePerformancePassed, string Status, string[] Failures)
{
    // App Present completion is not display scanout or unique-frame identity.
    // Offscreen probes may reject a bad cadence, but cannot certify display delivery.
    internal static MonitorPerformanceVerdict Evaluate(bool functionalPassed, IReadOnlyList<MonitorInterval> intervals)
    {
        var failures = new List<string>();
        if (!functionalPassed) failures.Add("Functional checks failed.");
        if (intervals.Count == 0) failures.Add("No measured presentation interval.");
        foreach (var sample in intervals)
        {
            if (!double.IsFinite(sample.Seconds) || sample.Seconds <= 0 || sample.Frames < 1 ||
                !double.IsFinite(sample.MaximumGapMs) || sample.MaximumGapMs <= 0)
            { failures.Add(sample.Surface + ": missing/invalid timing evidence."); continue; }
            if (sample.Frames < Math.Floor(sample.Seconds * 60) - 1)
                failures.Add(sample.Surface + ": fewer than 60 presentations/second (one endpoint allowance).");
            if (sample.MaximumGapMs * 1_000_000 > 16_666_667)
                failures.Add(sample.Surface + ": presentation interval exceeded the 60 Hz budget.");
        }
        return new(functionalPassed, failures.Count == 0, false,
            failures.Count == 0 ? "unverified-display-delivery" : "failed", failures.ToArray());
    }
}
