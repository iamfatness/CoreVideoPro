using System.Text.Json;

namespace CoreVideoPro.ShowEngine;

/// <summary>One declared parameter of an engine action. <c>Type</c> is the engine's own
/// <c>ActionParamType</c> wire string — "string" | "int" | "double" | "bool" — carried verbatim so the
/// bridge (Task 8) maps it to <c>ControlParamType</c> at the one place that knows that mapping.</summary>
public sealed record ShowEngineActionParam(string Name, string Type, bool Required, string Description);

/// <summary>One entry of the engine's <c>OHG_ACTIONS</c> manifest.</summary>
public sealed record ShowEngineActionDefinition(
    string Id,
    string Title,
    string Description,
    IReadOnlyList<ShowEngineActionParam> Params);

/// <summary>The engine's startup manifest — the <c>handshake</c> event (or an explicit handshake
/// response carrying the same payload). <c>Snapshot</c> and <c>Fields</c> are kept as raw
/// <see cref="JsonElement"/>: this project never models the ShowSnapshot shape, it forwards it.</summary>
public sealed record ShowEngineHandshake(
    int ProtocolVersion,
    string EngineVersion,
    int Generation,
    IReadOnlyList<ShowEngineActionDefinition> Actions,
    IReadOnlyList<string> FieldTemplates,
    JsonElement Snapshot,
    IReadOnlyDictionary<string, JsonElement> Fields);

/// <summary>A published <c>snapshot</c> event.</summary>
public sealed record ShowEngineSnapshot(
    int Generation,
    long Revision,
    JsonElement Snapshot,
    IReadOnlyDictionary<string, JsonElement> Fields);

/// <summary>A <c>hostCommand</c> event — the engine asking the shell to do something to the show.
/// <c>Args</c> is the raw JSON array; Task 10's adapter is what interprets it per command name.</summary>
public sealed record ShowEngineHostCommand(int Generation, long Seq, string Name, JsonElement Args);

/// <summary>A <c>log</c> event. <c>Level</c> is "info" | "warn" | "error".</summary>
public sealed record ShowEngineLogLine(string Level, string Message);

public enum ShowEngineState
{
    Stopped,
    Starting,
    Running,
    Recovering,
    Failed
}

/// <summary>The supervisor's whole observable condition. Immutable — every transition publishes a new
/// record, so a consumer that latched one can never see a half-written state.</summary>
public sealed record ShowEngineHealth(
    ShowEngineState State,
    int Generation,
    int RestartCount,
    string? LastError,
    DateTimeOffset? LastCrashAt);

/// <summary>The engine's <c>ActionResult</c> for an <c>invoke</c>: <c>Kind</c> is "ok" | "refused" |
/// "error". A refusal is NOT an error (see <c>show-engine/src/actions.ts</c>) — the engine correctly
/// saying no — so the two carry different payload fields and stay distinguishable to the caller.</summary>
public sealed record ShowEngineActionResult(string Kind, string? Reason, string? Message);
