using System.Text.Json;
using System.Text.Json.Serialization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Which telemetry event this is. Sent as the payload's <c>name</c> field, which
/// the telemetry-ingest worker indexes (S0 <c>handleEvent</c>).
/// </summary>
public static class TelemetryEventNames
{
    public const string SessionEnd = "session-end";
    public const string Heartbeat = "heartbeat";
}

/// <summary>
/// COUNTS / KINDS ONLY view of the production output configuration (spec §S3).
/// This is the whole no-secrets discipline: every field is a bool or an int
/// derived from the snapshot — there is NO field that can carry a stream key, an
/// endpoint URL, a participant name/id, a file path, or a meeting id.
/// </summary>
public sealed record TelemetryOutputConfigShape
{
    public bool RecordingEnabled { get; init; }
    public bool StreamingEnabled { get; init; }
    public bool VcamEnabled { get; init; }
    public int IsoSourceCount { get; init; }
    public int CaptureSourceCount { get; init; }
    public int ZoomParticipantCount { get; init; }
}

/// <summary>
/// The full telemetry event payload (spec §S3). Enumerable by design — every
/// field is listed here and nothing else is serialized, so the settings
/// "preview what's sent" affordance shows the operator the exact, complete
/// egress. Property names serialize to camelCase (see <see cref="TelemetryPayloadBuilder"/>).
/// </summary>
public sealed record TelemetryEventPayload
{
    /// <summary>Event kind — <c>session-end</c> or <c>heartbeat</c>.</summary>
    public required string Name { get; init; }

    /// <summary>App version (the D4/S1 way: package identity or assembly version).</summary>
    public required string Version { get; init; }

    /// <summary>Wall-clock length of the app session in whole seconds.</summary>
    public long SessionLengthSeconds { get; init; }

    public required TelemetryOutputConfigShape OutputConfigShape { get; init; }

    /// <summary>Crash reports recorded (S1 watermark) since the last telemetry send.</summary>
    public int CrashCountSinceLastSend { get; init; }

    /// <summary>
    /// S1-parity machine-class string (server-indexed). Kept as a top-level
    /// string so the ingest worker's <c>extractMachineClass</c> indexes it
    /// exactly like a crash report does.
    /// </summary>
    public required string MachineClass { get; init; }

    /// <summary>Structured, banded hardware detail (cores / RAM band / optional GPU band).</summary>
    public required TelemetryMachineClass Machine { get; init; }
}

/// <summary>
/// Pure builder for the S3 telemetry payload. Reads ONLY counts/kinds from the
/// media-core snapshot — it cannot, by construction, read a secret-bearing field
/// (no destination string, no stream key, no path, no name is ever touched). The
/// no-leak unit test seeds a snapshot full of secrets and asserts none appear in
/// the serialized JSON.
/// </summary>
public static class TelemetryPayloadBuilder
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = true
    };

    /// <summary>Compact (non-indented) form used on the wire.</summary>
    private static readonly JsonSerializerOptions WireOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = false
    };

    /// <summary>
    /// Derives the counts/kinds output shape from the latest snapshot. A null
    /// snapshot (engine off) yields all-false / zero — a valid, empty shape.
    /// </summary>
    public static TelemetryOutputConfigShape BuildOutputConfigShape(NativeMediaCoreStateSnapshot? snapshot)
    {
        if (snapshot is null)
        {
            return new TelemetryOutputConfigShape();
        }

        var captureSourceCount = snapshot.Frames
            .Where(static frame => frame.SourceId.StartsWith("capture:", StringComparison.Ordinal))
            .Select(static frame => frame.SourceId)
            .Distinct(StringComparer.Ordinal)
            .Count();

        return new TelemetryOutputConfigShape
        {
            // Recording is "enabled" when a session is actively writing.
            RecordingEnabled = snapshot.Recording is { Active: true },
            // Streaming is "enabled" when at least one output sender is active.
            StreamingEnabled = snapshot.OutputSenderSession.ActiveSenderCount > 0,
            VcamEnabled = snapshot.VirtualCamera.Enabled,
            IsoSourceCount = snapshot.IsoParticipantIds.Count,
            CaptureSourceCount = captureSourceCount,
            ZoomParticipantCount = snapshot.Participants.Count
        };
    }

    /// <summary>Assembles the complete payload from already-gathered inputs.</summary>
    public static TelemetryEventPayload Build(
        string eventName,
        string version,
        long sessionLengthSeconds,
        NativeMediaCoreStateSnapshot? snapshot,
        int crashCountSinceLastSend,
        TelemetryMachineClass machine) => new()
    {
        Name = eventName,
        Version = version,
        SessionLengthSeconds = Math.Max(0, sessionLengthSeconds),
        OutputConfigShape = BuildOutputConfigShape(snapshot),
        CrashCountSinceLastSend = Math.Max(0, crashCountSinceLastSend),
        MachineClass = machine.Label,
        Machine = machine
    };

    /// <summary>Indented JSON for the settings preview (auditable, human-readable).</summary>
    public static string SerializePreview(TelemetryEventPayload payload) =>
        JsonSerializer.Serialize(payload, SerializerOptions);

    /// <summary>Compact JSON for the wire (well under the 64KB event cap).</summary>
    public static string SerializeWire(TelemetryEventPayload payload) =>
        JsonSerializer.Serialize(payload, WireOptions);
}
