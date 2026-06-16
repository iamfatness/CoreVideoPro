namespace CoreVideoPro.WinUI.Models;

public enum LicenseTier
{
    Trial,
    Creator,
    Pro,
    Team
}

public enum LicenseStatus
{
    Trial,
    Active,
    Expired,
    Unlicensed
}

public sealed class LicenseState
{
    public LicenseTier Tier { get; init; } = LicenseTier.Trial;
    public LicenseStatus Status { get; init; } = LicenseStatus.Trial;
    public long? TrialEndsAtMs { get; init; }
    public string? AccountEmail { get; init; }
}

public sealed class Entitlements
{
    public int MaxZoomParticipants { get; init; }
    public int MaxOutputHeight { get; init; }
    public bool Watermark { get; init; }
    public bool SetAndForget { get; init; }
    public bool ChromaKey { get; init; }
    public bool Phase2Outputs { get; init; }
}

public static class LicenseCatalog
{
    public static string TierLabel(LicenseTier tier) => tier switch
    {
        LicenseTier.Trial => "Free Trial",
        LicenseTier.Creator => "Creator",
        LicenseTier.Pro => "Pro",
        LicenseTier.Team => "Team",
        _ => tier.ToString()
    };

    public static Entitlements DeriveEntitlements(LicenseState state, long nowMs)
    {
        if (state.Status is LicenseStatus.Expired or LicenseStatus.Unlicensed)
        {
            return new Entitlements
            {
                MaxZoomParticipants = 2,
                MaxOutputHeight = 720,
                Watermark = true,
                SetAndForget = false,
                ChromaKey = false,
                Phase2Outputs = false
            };
        }

        if (state.Status == LicenseStatus.Trial)
        {
            if (state.TrialEndsAtMs is long trialEnd && nowMs >= trialEnd)
            {
                return DeriveEntitlements(new LicenseState { Status = LicenseStatus.Expired }, nowMs);
            }

            return new Entitlements
            {
                MaxZoomParticipants = 8,
                MaxOutputHeight = 720,
                Watermark = true,
                SetAndForget = true,
                ChromaKey = true,
                Phase2Outputs = true
            };
        }

        return state.Tier switch
        {
            LicenseTier.Creator => new Entitlements
            {
                MaxZoomParticipants = 4,
                MaxOutputHeight = 1080,
                Watermark = false,
                SetAndForget = false,
                ChromaKey = false,
                Phase2Outputs = false
            },
            LicenseTier.Pro or LicenseTier.Team => new Entitlements
            {
                MaxZoomParticipants = 8,
                MaxOutputHeight = 1080,
                Watermark = false,
                SetAndForget = true,
                ChromaKey = true,
                Phase2Outputs = true
            },
            _ => new Entitlements
            {
                MaxZoomParticipants = 8,
                MaxOutputHeight = 720,
                Watermark = true,
                SetAndForget = true,
                ChromaKey = true,
                Phase2Outputs = true
            }
        };
    }
}