using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Allowlisted support view of the existing generated core observation.</summary>
public sealed record MonitorIsolationObservation
{
    public string RequestedMode { get; init; } = "unknown";
    public string EffectiveMode { get; init; } = "unknown";
    public string SelectionSource { get; init; } = "unknown";
    public string Readiness { get; init; } = "unavailable";
    public string FailureReason { get; init; } = "unknown";
    public long? DeliveryEpoch { get; init; }
    public bool? WorkerExists { get; init; }
    public long? ReadyInputs { get; init; }
    public long? HeldInputs { get; init; }
    public long? UnavailableInputs { get; init; }
    public long? RetainedInputs { get; init; }
    public long? RetainedInputBytes { get; init; }
    public long? RetentionRefusals { get; init; }

    public static MonitorIsolationObservation FromSnapshot(NativeMediaCoreStateSnapshot? snapshot)
    {
        if (string.IsNullOrWhiteSpace(snapshot?.RawJson)) return new();
        try
        {
            var evidence = CoreObservationModel.Parse(snapshot.RawJson).RealtimeEvidence;
            if (evidence is not { ValueKind: JsonValueKind.Object } node ||
                !node.TryGetProperty("monitorWorker", out var worker) || worker.ValueKind != JsonValueKind.Object)
                return new();
            // Never infer readiness from the legacy enabled flag or accept arbitrary
            // error strings into the bundle. Older peers remain explicitly unknown.
            var inputObserved = Known(worker, "inputObservationVersion", "unknown", "monitor-input-admission-v1") == "monitor-input-admission-v1" &&
                worker.TryGetProperty("enabled", out var exists) && exists.ValueKind == JsonValueKind.True &&
                Count(worker, "completed") is > 0;
            return new()
            {
                ReadyInputs = inputObserved ? Count(worker, "readyInputs") : null,
                HeldInputs = inputObserved ? Count(worker, "heldInputs") : null,
                UnavailableInputs = inputObserved ? Count(worker, "unavailableInputs") : null,
                RetainedInputs = inputObserved ? Count(worker, "retainedInputs") : null,
                RetainedInputBytes = inputObserved ? Count(worker, "retainedInputBytes") : null,
                RetentionRefusals = inputObserved ? Count(worker, "retentionRefusals") : null,
                RequestedMode = Known(worker, "requestedMode", "unknown", "inline", "isolated", "invalid"),
                EffectiveMode = Known(worker, "effectiveMode", "unknown", "inline", "isolated"),
                SelectionSource = Known(worker, "selectionSource", "unknown", "default", "override", "constructor"),
                Readiness = Known(worker, "readiness", "unavailable", "starting", "ready", "degraded"),
                FailureReason = Known(worker, "failureReason", "unknown", "", "monitor-initialization", "monitor-render", "monitor-frame-admission", "invalid-monitor-override"),
                DeliveryEpoch = worker.TryGetProperty("deliveryEpoch", out var epoch) && epoch.ValueKind == JsonValueKind.Number && epoch.TryGetInt64(out var number) && number >= 0 ? number : null,
                WorkerExists = worker.TryGetProperty("enabled", out var enabled) && enabled.ValueKind is JsonValueKind.True or JsonValueKind.False ? enabled.GetBoolean() : null
            };
        }
        catch (JsonException) { return new(); }
    }

    private static long? Count(JsonElement node, string key) =>
        node.TryGetProperty(key, out var value) && value.ValueKind == JsonValueKind.Number &&
        value.TryGetDecimal(out var number) && number >= 0 && number <= 9007199254740991m && decimal.Truncate(number) == number
            ? (long)number : null;

    private static string Known(JsonElement node, string key, string fallback, params string[] allowed)
    {
        if (!node.TryGetProperty(key, out var value) || value.ValueKind != JsonValueKind.String) return fallback;
        var text = value.GetString();
        return text is not null && allowed.Contains(text, StringComparer.Ordinal) ? text : fallback;
    }
}
