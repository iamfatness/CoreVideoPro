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
}
