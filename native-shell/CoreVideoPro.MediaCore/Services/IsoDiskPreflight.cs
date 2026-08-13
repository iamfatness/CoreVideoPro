using System.Globalization;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Disk pre-flight verdict for arming a (program + ISO) recording.
/// <list type="bullet">
/// <item><see cref="Ample"/> — plenty of headroom; arm silently.</item>
/// <item><see cref="Low"/> — enough to start but the operator should be warned
/// BEFORE arming (never a mid-show surprise); arming still proceeds.</item>
/// <item><see cref="Insufficient"/> — can't fit a sane minimum recording; block
/// arming with a clear message.</item>
/// </list>
/// </summary>
public enum IsoDiskPreflightLevel
{
    Ample,
    Low,
    Insufficient
}

/// <summary>
/// The result of a disk pre-flight estimate: the combined write rate, how much the
/// target volume can hold, and an operator-facing message for the low/insufficient
/// cases.
/// </summary>
public sealed record IsoDiskPreflightResult
{
    public required IsoDiskPreflightLevel Level { get; init; }

    /// <summary>Program bitrate + Σ selected ISO bitrates (Mbps).</summary>
    public double CombinedBitrateMbps { get; init; }

    /// <summary>Free space on the target volume, GB (decimal, 1e9 bytes).</summary>
    public double AvailableGb { get; init; }

    /// <summary>Estimated space the planning window would consume, GB.</summary>
    public double RequiredGbForPlanning { get; init; }

    /// <summary>Estimated recording time remaining at the combined rate, seconds.</summary>
    public long RemainingSeconds { get; init; }

    /// <summary>Number of ISO sources folded into the estimate.</summary>
    public int IsoSourceCount { get; init; }

    /// <summary>Operator-facing message (empty when <see cref="IsoDiskPreflightLevel.Ample"/>).</summary>
    public string Message { get; init; } = string.Empty;

    /// <summary>True only for the genuinely-insufficient case — arming must be refused.</summary>
    public bool ShouldBlock => Level == IsoDiskPreflightLevel.Insufficient;

    /// <summary>True for the low-space case — warn the operator but let them proceed.</summary>
    public bool ShouldWarn => Level == IsoDiskPreflightLevel.Low;
}

/// <summary>
/// Pure disk pre-flight math for the recording-arm path (ISO-4, spec §6). Ports the
/// estimate from <c>src/engine/isoRecording.ts</c> / <c>diskSpace.ts</c>: required
/// space = (program + Σ selected ISO bitrates) × duration vs free space on the target
/// volume. Low space → loud warning BEFORE arming; genuinely-insufficient (can't fit a
/// sane minimum) → block. Never silent; never blocks on a merely-marginal estimate.
///
/// No native protocol or snapshot field is involved — the shell already knows the
/// target folder, the program bitrate, and the selected ISO source ids, and queries
/// free space with <see cref="System.IO.DriveInfo"/>, so this stays entirely shell-side.
/// </summary>
public static class IsoDiskPreflight
{
    /// <summary>
    /// Per-ISO write rate estimate (Mbps): a 1080p H.264 ISO video track (6 Mbps, matching
    /// <c>isoRecording.ts</c> BITRATE_BY_TIER["1080p"]) plus one raw-stem AAC track
    /// (~192 kbps, matching <c>diskSpace.ts</c> estimateDiskRateMbps). Video-only ISOs
    /// over-estimate by the audio share, which is the safe direction for a pre-flight.
    /// </summary>
    public const double DefaultPerIsoMbps = 6.192;

    /// <summary>
    /// Planning window used to size the "low space" warning. This is a soft sizing
    /// horizon, NOT a hard limit — a longer show is still allowed to arm.
    /// </summary>
    public const int DefaultPlanningMinutes = 30;

    /// <summary>
    /// Below this many minutes of headroom the volume genuinely cannot hold a sane
    /// minimum recording, so arming is refused.
    /// </summary>
    public const int DefaultSaneMinimumMinutes = 5;

    /// <summary>Byte-per-decimal-GB (matches the TS math's 1e9 convention).</summary>
    private const double BytesPerGb = 1e9;

    /// <summary>
    /// Evaluate the disk pre-flight for a (program + N ISO) recording against the free
    /// bytes on the target volume.
    /// </summary>
    /// <param name="programBitrateMbps">The program recording bitrate (Mbps).</param>
    /// <param name="isoSourceCount">Number of ISO sources that will arm writers.</param>
    /// <param name="availableBytes">Free bytes on the target volume (DriveInfo.AvailableFreeSpace).</param>
    public static IsoDiskPreflightResult Evaluate(
        double programBitrateMbps,
        int isoSourceCount,
        long availableBytes,
        double perIsoMbps = DefaultPerIsoMbps,
        int planningMinutes = DefaultPlanningMinutes,
        int saneMinimumMinutes = DefaultSaneMinimumMinutes)
    {
        var isoCount = Math.Max(0, isoSourceCount);
        var program = Math.Max(0.0, programBitrateMbps);
        var perIso = Math.Max(0.0, perIsoMbps);
        var combinedMbps = program + isoCount * perIso;

        // Mbps → bytes/s. Decimal megabits, matching the TS estimate (Mbps * 1e6 / 8).
        var rateBytesPerSec = combinedMbps * 1_000_000.0 / 8.0;
        var available = Math.Max(0L, availableBytes);
        var availableGb = available / BytesPerGb;

        var remainingSeconds = rateBytesPerSec <= 0.0
            ? long.MaxValue
            : (long)Math.Floor(available / rateBytesPerSec);

        var requiredGbForPlanning = rateBytesPerSec * planningMinutes * 60.0 / BytesPerGb;
        var saneMinimumSeconds = (long)saneMinimumMinutes * 60L;
        var planningSeconds = (long)planningMinutes * 60L;

        IsoDiskPreflightLevel level;
        string message;

        var isoSuffix = isoCount > 0 ? $" + {isoCount} ISO" : string.Empty;
        var rateLabel = $"{combinedMbps.ToString("0.#", CultureInfo.InvariantCulture)} Mbps (program{isoSuffix})";

        if (available <= 0 || remainingSeconds < saneMinimumSeconds)
        {
            level = IsoDiskPreflightLevel.Insufficient;
            message =
                $"Not enough disk space to record: only {FormatGb(availableGb)} free at {rateLabel} — " +
                $"under {saneMinimumMinutes} min of headroom. Free space" +
                (isoCount > 0 ? " or reduce ISO sources" : string.Empty) +
                " before arming.";
        }
        else if (remainingSeconds < planningSeconds || requiredGbForPlanning > availableGb)
        {
            level = IsoDiskPreflightLevel.Low;
            message =
                $"Low disk space: {FormatGb(availableGb)} free at {rateLabel} — " +
                $"about {FormatRemaining(remainingSeconds)} of recording left. Free space before a long show.";
        }
        else
        {
            level = IsoDiskPreflightLevel.Ample;
            message = string.Empty;
        }

        return new IsoDiskPreflightResult
        {
            Level = level,
            CombinedBitrateMbps = combinedMbps,
            AvailableGb = availableGb,
            RequiredGbForPlanning = requiredGbForPlanning,
            RemainingSeconds = remainingSeconds,
            IsoSourceCount = isoCount,
            Message = message
        };
    }

    private static string FormatGb(double gb) =>
        $"{gb.ToString("0.0", CultureInfo.InvariantCulture)} GB";

    /// <summary>Human-readable remaining-time label (mirrors diskSpace.ts formatRemainingTime).</summary>
    public static string FormatRemaining(long seconds)
    {
        if (seconds == long.MaxValue || seconds > 86400L * 7)
        {
            return "7+ days";
        }

        if (seconds >= 3600)
        {
            var h = seconds / 3600;
            var m = seconds % 3600 / 60;
            return $"{h}h {m}m";
        }

        if (seconds >= 60)
        {
            return $"{seconds / 60} min";
        }

        return $"{seconds} sec";
    }
}
