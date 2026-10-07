using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Bounded support view of the shell BGRA preparation boundary.</summary>
public sealed record CapturePreparationObservation
{
    public bool? Enabled { get; init; }
    public string State { get; init; } = "unavailable";
    public string Reason { get; init; } = "unknown";
    public string LastRefusalReason { get; init; } = "unknown";
    public string MemoryAccounting { get; init; } = "unknown";
    public long? Accepted { get; init; }
    public long? Refused { get; init; }
    public long? Prepared { get; init; }
    public long? Torn { get; init; }
    public long? PoolBusy { get; init; }
    public long? Failed { get; init; }
    public long? ResidentBytes { get; init; }
    public long? BudgetBytes { get; init; }
    public long? Active { get; init; }
    public long? Retiring { get; init; }
    public long? CopyTotalNs { get; init; }
    public long? CopyMaximumNs { get; init; }
    public bool? SenderAcquisitionTimeVerified { get; init; }

    public static CapturePreparationObservation FromSnapshot(NativeMediaCoreStateSnapshot? snapshot)
    {
        if (string.IsNullOrWhiteSpace(snapshot?.RawJson)) return new();
        try
        {
            var evidence = CoreObservationModel.Parse(snapshot.RawJson).RealtimeEvidence;
            if (evidence is not { ValueKind: JsonValueKind.Object } node ||
                !node.TryGetProperty("capturePreparation", out var capture) || capture.ValueKind != JsonValueKind.Object ||
                Known(capture, "version", "unknown", "shm-preparation-v1") != "shm-preparation-v1" ||
                Known(capture, "kind", "unknown", "winui-shared-memory") != "winui-shared-memory") return new();
            bool? enabled = capture.TryGetProperty("enabled", out var flag) && flag.ValueKind is JsonValueKind.True or JsonValueKind.False
                ? flag.GetBoolean() : null;
            var accounting = Known(capture, "memoryAccounting", "unknown", "mapped-payload-plus-four-cpu-frames");
            long? Counter(string key) => enabled == true ? Number(capture, key) : null;
            long? Resource(string key) => accounting == "unknown" ? null : Counter(key);
            return new()
            {
                Enabled = enabled,
                State = enabled == true ? Known(capture, "state", "unknown", "idle", "warming", "ready", "degraded", "unavailable") : "unavailable",
                Reason = ReasonValue(capture, "reason"),
                LastRefusalReason = ReasonValue(capture, "lastRefusalReason"),
                MemoryAccounting = accounting,
                Accepted = Counter("accepted"), Refused = Counter("refused"), Prepared = Counter("prepared"),
                Torn = Counter("torn"), PoolBusy = Counter("poolBusy"), Failed = Counter("failed"),
                Active = Counter("active"), Retiring = Counter("retiring"),
                ResidentBytes = Resource("residentBytes"), BudgetBytes = Resource("budgetBytes"),
                CopyTotalNs = Counter("copyTotalNs"), CopyMaximumNs = Counter("copyMaximumNs"),
                // This protocol version supplies an observation boundary only.
                // A peer's unexpected true flag cannot establish acquisition proof.
                SenderAcquisitionTimeVerified = capture.TryGetProperty("senderAcquisitionTimeVerified", out var acquisition) &&
                    acquisition.ValueKind == JsonValueKind.False ? false : null
            };
        }
        catch (JsonException) { return new(); }
    }

    private static long? Number(JsonElement node, string key)
    {
        if (!node.TryGetProperty(key, out var value) || value.ValueKind != JsonValueKind.Number ||
            !value.TryGetDecimal(out var number) || number < 0 ||
            number > 9_007_199_254_740_991m || decimal.Truncate(number) != number) return null;
        return (long)number;
    }

    private static string ReasonValue(JsonElement node, string key) => Known(node, key, "unknown", "",
        "invalid-mapping", "capture-budget", "capture-source-capacity", "capture-retirement-pending",
        "capture-mapping-unavailable", "capture-allocation", "capture-dimensions", "capture-pool-busy", "capture-copy");

    private static string Known(JsonElement node, string key, string fallback, params string[] allowed)
    {
        if (!node.TryGetProperty(key, out var value) || value.ValueKind != JsonValueKind.String) return fallback;
        var text = value.GetString();
        return text is not null && allowed.Contains(text, StringComparer.Ordinal) ? text : fallback;
    }
}
