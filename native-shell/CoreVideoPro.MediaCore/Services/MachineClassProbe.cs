namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Structured machine class for telemetry (spec §S3). The <see cref="Label"/>
/// is the SAME <c>win-x64-cpu&lt;N&gt;-ram&lt;N&gt;gb</c> string S1 already sends
/// as crash metadata (single source of truth — <see cref="MachineClassProbe.Describe"/>);
/// the banded fields are for the telemetry event payload. NO identifiers, no
/// machine name, no serials — coarse hardware bands only.
/// </summary>
public sealed record TelemetryMachineClass
{
    /// <summary>S1-parity label, e.g. <c>win-x64-cpu16-ram32gb</c>. Server-indexed.</summary>
    public required string Label { get; init; }

    public int CpuCores { get; init; }

    /// <summary>Coarse RAM band, e.g. <c>16-32GB</c>. Never the exact byte count.</summary>
    public required string RamBand { get; init; }

    /// <summary>
    /// Best-effort coarse GPU descriptor (e.g. an adapter-name band). Null when
    /// the (platform-specific) probe is unavailable — MediaCore is portable
    /// net9.0, so the shell supplies this; the payload stays valid without it.
    /// </summary>
    public string? GpuTier { get; init; }
}

/// <summary>
/// Portable hardware probe. Reused by BOTH the S1 crash pipeline (the
/// <see cref="Describe"/> label) and the S3 telemetry payload (the banded
/// <see cref="TelemetryMachineClass"/>), so the machine-class string is computed
/// in exactly one place. Deliberately dependency-free (Environment + GC only) so
/// it lives in portable MediaCore and is unit-testable without Windows.
/// </summary>
public static class MachineClassProbe
{
    /// <summary>
    /// The S1 crash-metadata label. Identical shape to the string
    /// <c>CrashReportCoordinator</c> historically produced —
    /// <c>win-x64-cpu&lt;cores&gt;-ram&lt;gb&gt;gb</c> — so extracting it here
    /// de-duplicates rather than forks the machine-class string.
    /// </summary>
    public static string Describe()
    {
        try
        {
            var ramGb = RamGigabytes();
            return $"win-x64-cpu{Environment.ProcessorCount}-ram{ramGb}gb";
        }
        catch
        {
            return "win-x64";
        }
    }

    /// <summary>
    /// Structured, banded machine class for the telemetry payload.
    /// <paramref name="gpuTier"/> is the shell's optional best-effort GPU band.
    /// </summary>
    public static TelemetryMachineClass Probe(string? gpuTier = null) => new()
    {
        Label = Describe(),
        CpuCores = Environment.ProcessorCount,
        RamBand = RamBand(RamGigabytes()),
        GpuTier = string.IsNullOrWhiteSpace(gpuTier) ? null : gpuTier.Trim()
    };

    private static int RamGigabytes() =>
        (int)Math.Round(GC.GetGCMemoryInfo().TotalAvailableMemoryBytes / (1024.0 * 1024 * 1024));

    /// <summary>Buckets exact RAM into coarse bands so nothing machine-identifying leaks.</summary>
    public static string RamBand(int ramGb) => ramGb switch
    {
        <= 0 => "unknown",
        < 8 => "<8GB",
        < 16 => "8-16GB",
        < 32 => "16-32GB",
        < 64 => "32-64GB",
        < 128 => "64-128GB",
        _ => "128GB+"
    };
}
