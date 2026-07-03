using System.Text.Json;
using System.Text.Json.Serialization;
using CoreVideoPro.Control.Osc;

namespace CoreVideoPro.Control;

public sealed record ControlManifestParam(string Name, string Type, bool Required, string? Description);

public sealed record ControlManifestAction(
    string Id,
    string Title,
    string Description,
    string OscAddress,
    IReadOnlyList<ControlManifestParam> Params);

/// <summary>Machine-readable description of the whole control API — every action, its params, and
/// its OSC address — plus the feedback address space. This is the contract the Bitfocus Companion
/// module is generated/validated from, and what a "GET /manifest" HTTP endpoint would serve. Keeping
/// it derived from <see cref="ControlActionRegistry"/> means the manifest can never drift from the
/// live action table.</summary>
public sealed record ControlManifest(
    int Version,
    string Root,
    IReadOnlyList<ControlManifestAction> Actions,
    IReadOnlyList<string> FeedbackFields)
{
    public const int SchemaVersion = 1;

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    // Feedback fields mirror ControlState / OscFeedback (per-input fields are documented as a
    // template since the slot index is dynamic).
    public static readonly IReadOnlyList<string> StateFields = new[]
    {
        "recording", "streaming", "engineOn", "canTake", "lowerThirdOnAir",
        "zoomStatus", "engineStatus", "commandStatus", "activeSceneId", "previewSceneId", "viewMode",
        "multiviewLayoutMode", "multiviewTileCount",
        "automationOn", "autoTake", "autoAssignInputs", "autoLowerThirds", "autoCaptions",
        "audioMonitorOn", "audioMonitorVolume", "masterLimiterOn",
        "input/{slot}/inShow", "input/{slot}/kind", "input/{slot}/source", "input/{slot}/name"
    };

    public static ControlManifest Build(OscAddressMap? addressMap = null)
    {
        var map = addressMap ?? new OscAddressMap();
        var actions = ControlActionRegistry.Actions
            .Select(action => new ControlManifestAction(
                action.Id,
                action.Title,
                action.Description,
                map.ActionIdToAddress(action.Id),
                action.Params
                    .Select(p => new ControlManifestParam(p.Name, p.Type.ToString().ToLowerInvariant(), p.Required, p.Description))
                    .ToList()))
            .ToList();

        return new ControlManifest(SchemaVersion, map.Root, actions, StateFields);
    }

    public string ToJson() => JsonSerializer.Serialize(this, JsonOptions);
}
