namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// Shared console-meter calibration. The UI's 0-100 meter value represents a
/// linear visual position from -60 dBFS to 0 dBFS; the same contract drives
/// both the fill and the printed hash marks.
/// </summary>
public static class AudioMeterScale
{
    public const double MinimumDbfs = -60;
    public const double MaximumDbfs = 0;

    public static IReadOnlyList<double> MajorTicksDbfs { get; } =
        [0, -6, -12, -24, -36, -48, -60];

    public static int ToLevel(double dbfs, bool muted = false)
    {
        if (muted || !double.IsFinite(dbfs) || dbfs <= MinimumDbfs)
        {
            return 0;
        }

        var normalized = (dbfs - MinimumDbfs) / (MaximumDbfs - MinimumDbfs);
        return Math.Clamp((int)Math.Round(normalized * 100), 0, 100);
    }

    /// <summary>
    /// Fits a segmented vertical meter to its current viewport. Full-size
    /// consoles keep the requested resolution and grow the segment bodies;
    /// compact windows reduce the segment count only when 2 px bodies no
    /// longer fit. The returned stack always occupies the available height.
    /// </summary>
    public static AudioMeterSegmentLayout FitVerticalSegments(double availableHeight, int requestedCount)
    {
        var count = Math.Clamp(requestedCount, 8, 48);
        if (!double.IsFinite(availableHeight) || availableHeight <= 0)
        {
            return new AudioMeterSegmentLayout(count, 7, 2);
        }

        const double minimumSegmentSize = 2;
        var spacing = availableHeight / count >= 5 ? 2d : 1d;
        var segmentSize = (availableHeight - spacing * (count - 1)) / count;

        if (segmentSize < minimumSegmentSize)
        {
            spacing = 1;
            count = Math.Min(count, Math.Max(1,
                (int)Math.Floor((availableHeight + spacing) / (minimumSegmentSize + spacing))));

            if (count == 1)
            {
                spacing = 0;
            }

            segmentSize = Math.Max(0,
                (availableHeight - spacing * (count - 1)) / count);
        }

        return new AudioMeterSegmentLayout(count, segmentSize, spacing);
    }
}

public readonly record struct AudioMeterSegmentLayout(int SegmentCount, double SegmentSize, double Spacing)
{
    public double OccupiedSize =>
        SegmentCount * SegmentSize + Math.Max(0, SegmentCount - 1) * Spacing;
}
