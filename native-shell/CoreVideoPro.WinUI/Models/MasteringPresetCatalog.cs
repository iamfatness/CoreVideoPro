namespace CoreVideoPro.WinUI.Models;

/// <summary>
/// A complete, atomic snapshot of the built-in master-bus processor.
/// Keeping this as data lets presets and A/B comparison update the native DSP
/// in one operation instead of firing a separate media-core sync per control.
/// B1 glue dynamics default to the pre-B1 fixed values (2:1, 30/250 ms, no
/// makeup, single-band) so neutral settings stay bit-identical to old behavior.
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
    bool LimiterEnabled,
    double GlueRatio = 2.0,
    double GlueAttackMs = 30.0,
    double GlueReleaseMs = 250.0,
    double GlueMakeupDb = 0.0,
    bool GlueMultiband = false,
    double GlueBandLowDb = 0.0,
    double GlueBandMidDb = 0.0,
    double GlueBandHighDb = 0.0);

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

    // Stage ids follow the DSP chain order (B2 rack): input, filter, tone,
    // ride, glue, width, ceiling. The pre-B2 aliases (dynamics = glue,
    // loudness/image = ride+width, limiter = ceiling) stay accepted so nothing
    // that scripted the old ids breaks.
    public static MasteringSettings ResetStage(MasteringSettings current, string stageId) =>
        stageId.Trim().ToLowerInvariant() switch
        {
            "input" => current with { InputGainDb = 0.0 },
            "filter" => current with { HighPassHz = 0.0, LowPassHz = 0.0 },
            "tone" => current with { LowShelfDb = 0.0, PresenceDb = 0.0, HighShelfDb = 0.0 },
            "ride" => current with { MaxRideDb = 0.0 },
            "glue" or "dynamics" => current with
            {
                GlueAmount = 0.0,
                GlueRatio = 2.0,
                GlueAttackMs = 30.0,
                GlueReleaseMs = 250.0,
                GlueMakeupDb = 0.0,
                GlueMultiband = false,
                GlueBandLowDb = 0.0,
                GlueBandMidDb = 0.0,
                GlueBandHighDb = 0.0
            },
            "width" => current with { StereoWidth = 1.0 },
            "loudness" or "image" => current with { MaxRideDb = 0.0, StereoWidth = 1.0 },
            "ceiling" or "limiter" => current with { CeilingDbfs = -1.3, LimiterEnabled = true },
            _ => throw new ArgumentOutOfRangeException(nameof(stageId), stageId, "Unknown mastering stage.")
        };
}
