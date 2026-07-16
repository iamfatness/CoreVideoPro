namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// A complete, atomic snapshot of the built-in master-bus processor.
/// Keeping this as data lets presets and A/B comparison update the native DSP
/// in one operation instead of firing a separate media-core sync per control.
/// </summary>
public sealed record MasteringSettings(
    bool Enabled,
    int TargetIndex,
    double GlueAmount,
    double CeilingDbfs,
    double MaxRideDb,
    double InputGainDb,
    double HighPassHz,
    double LowPassHz,
    double LowShelfDb,
    double PresenceDb,
    double HighShelfDb,
    double StereoWidth,
    bool LimiterEnabled);

public static class MasteringPresetCatalog
{
    public static MasteringSettings Create(string presetId) => presetId.Trim().ToLowerInvariant() switch
    {
        "streaming" => new(true, 0, 0.35, -1.0, 6.0, 0.0, 30.0, 0.0, 0.5, 1.0, 0.5, 1.05, true),
        "podcast" => new(true, 1, 0.45, -1.0, 8.0, 0.0, 70.0, 0.0, 0.0, 1.5, 0.5, 1.0, true),
        "broadcast" => new(true, 2, 0.30, -1.0, 4.0, 0.0, 30.0, 0.0, 0.0, 0.5, 0.0, 1.0, true),
        "neutral" => Neutral,
        _ => throw new ArgumentOutOfRangeException(nameof(presetId), presetId, "Unknown mastering preset.")
    };

    public static MasteringSettings Neutral =>
        new(true, 0, 0.0, -1.3, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, true);

    public static MasteringSettings ResetStage(MasteringSettings current, string stageId) =>
        stageId.Trim().ToLowerInvariant() switch
        {
            "input" => current with { InputGainDb = 0.0 },
            "filter" => current with { HighPassHz = 0.0, LowPassHz = 0.0 },
            "tone" => current with { LowShelfDb = 0.0, PresenceDb = 0.0, HighShelfDb = 0.0 },
            "dynamics" => current with { GlueAmount = 0.0 },
            "loudness" or "image" => current with { MaxRideDb = 0.0, StereoWidth = 1.0 },
            "limiter" => current with { CeilingDbfs = -1.3, LimiterEnabled = true },
            _ => throw new ArgumentOutOfRangeException(nameof(stageId), stageId, "Unknown mastering stage.")
        };
}
