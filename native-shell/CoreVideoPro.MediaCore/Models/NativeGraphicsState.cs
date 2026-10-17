using CoreVideoPro.MediaCore.Contracts;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>Native accepted graphics revisions; no shell-derived on-air state.</summary>
public sealed record NativeGraphicsState
{
    public int SchemaVersion { get; init; }
    public string CoreEpoch { get; init; } = "";
    public IReadOnlyList<NativeGraphicsInstance> Instances { get; init; } = [];
    public NativeGraphicsOperationResult? LastResult { get; init; }
    public IReadOnlyList<NativeGraphicsSource> Sources { get; init; } = [];
    public string SourceBindingStatus { get; init; } = "unavailable";
    public NativeGraphicsResourceBudget? ResourceBudgetAtStatePublication { get; init; }
    public IReadOnlyList<NativeGraphicsTemplate> Templates { get; init; } = [];
    public IReadOnlyList<NativeGraphicsTemplateInstall> TemplateInstalls { get; init; } = [];
}

public sealed record NativeGraphicsTemplateInstall
{
    public string ClientId { get; init; } = "";
    public string RequestId { get; init; } = "";
    public string State { get; init; } = "";
    public string Reason { get; init; } = "";
}

public sealed record NativeGraphicsTemplate
{
    public int SchemaVersion { get; init; }
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public string Name { get; init; } = "";
    public string Family { get; init; } = "";
    public string Runtime { get; init; } = "";
    public string ManifestHash { get; init; } = "";
    public JsonElement? Definition { get; init; }
    public JsonElement? Canvas { get; init; }
    public JsonElement? Bounds { get; init; }
    public IReadOnlyList<NativeGraphicsTemplateField> Fields { get; init; } = [];
    public JsonElement? Layers { get; init; }
    public IReadOnlyList<string> BrandTokens { get; init; } = [];
    public NativeGraphicsAnimations Animations { get; init; } = new();
    [JsonExtensionData] public Dictionary<string, JsonElement>? ExtensionData { get; init; }
}

public sealed record NativeGraphicsTemplateField
{
    public string Id { get; init; } = "";
    public string Type { get; init; } = "";
    public JsonElement? Default { get; init; }
    public bool Required { get; init; }
    public bool Bindable { get; init; }
    public int? MaxLength { get; init; }
    public double? Minimum { get; init; }
    public double? Maximum { get; init; }
    public IReadOnlyList<JsonElement> Choices { get; init; } = [];
    [JsonExtensionData] public Dictionary<string, JsonElement>? ExtensionData { get; init; }
}

/// <summary>Allocation-model counters sampled when native graphics state publishes.
/// They do not measure driver overhead or output delivery.</summary>
public sealed record NativeGraphicsResourceBudget
{
    public string Kind { get; init; } = "";
    public long CpuBytes { get; init; }
    public long GpuBytes { get; init; }
    public long ActivePreparations { get; init; }
    public long CpuHighWater { get; init; }
    public long GpuHighWater { get; init; }
    public long PreparationHighWater { get; init; }
    public long Refused { get; init; }
    public long CpuLimit { get; init; }
    public long GpuLimit { get; init; }
    public long PreparationLimit { get; init; }
}

public sealed record NativeGraphicsSource
{
    public SourceInstanceIdentity? Identity { get; init; }
    public string DisplayName { get; init; } = "";
    public bool VideoOn { get; init; }
}

public sealed record NativeGraphicsInstance
{
    public GraphicsRevisionIdentity? Identity { get; init; }
    public GraphicsPreparationFact? PreparationObservation { get; init; }
    public GraphicsCompositionFact? FirstCompositionObservation { get; init; }
    public string InstanceId { get; init; } = "";
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public string Scope { get; init; } = "";
    public string SceneId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public SourceInstanceIdentity? Binding { get; init; }
    public SourceInstanceIdentity? ProgramBinding { get; init; }
    public string BindingState { get; init; } = "unbound";
    public long Revision { get; init; }
    public long ProgramRevision { get; init; }
    public long CueRevision { get; init; }
    public long CueRequestedRevision { get; init; }
    public bool CuePending { get; init; }
    public string CuePreparation { get; init; } = "unprepared";
    public string CueWarning { get; init; } = "";
    public NativeGraphicsFields? CueFields { get; init; }
    public LowerThirdAppearance? CueStyle { get; init; }
    public NativeGraphicsAnimations? CueAnimations { get; init; }
    public SourceInstanceIdentity? CueBinding { get; init; }
    public NativeGraphicsResource? CueResource { get; init; }
    public string Preparation { get; init; } = "unprepared";
    public NativeGraphicsResource? PreparedResource { get; init; }
    public NativeGraphicsResource? ProgramResource { get; init; }
    public string Visibility { get; init; } = "hidden";
    public bool Cued { get; init; }
    public bool ShowPending { get; init; }
    /// <summary>First native draw submission. Does not establish receiver delivery.</summary>
    public long FirstDrawSubmittedFrame { get; init; }
    public string Warning { get; init; } = "";
    public NativeGraphicsFields Fields { get; init; } = new();
    public LowerThirdAppearance Style { get; init; } = new();
    public NativeGraphicsAnimations Animations { get; init; } = new();
    public string ProgramSourceId { get; init; } = "";
    public NativeGraphicsFields? ProgramFields { get; init; }
    public LowerThirdAppearance? ProgramStyle { get; init; }
    public NativeGraphicsAnimations? ProgramAnimations { get; init; }
}

public sealed record NativeGraphicsResource
{
    public string Backend { get; init; } = "";
    public int Width { get; init; }
    public int Height { get; init; }
    public string ContentKey { get; init; } = "";
}

public sealed record NativeGraphicsAnimations
{
    public NativeGraphicsTransition In { get; init; } = new();
    public NativeGraphicsTransition Out { get; init; } = new();
}

public sealed record NativeGraphicsTransition
{
    public string Type { get; init; } = "none";
    public string Direction { get; init; } = "left";
    public string Easing { get; init; } = "linear";
    public double DurationMs { get; init; }
}

public sealed record NativeGraphicsFields
{
    public string Name { get; init; } = "";
    public string Title { get; init; } = "";
    [JsonExtensionData] public Dictionary<string, JsonElement>? AdditionalFields { get; init; }
}

public sealed record NativeGraphicsOperationResult
{
    public string RequestId { get; init; } = "";
    public bool Accepted { get; init; }
    public string Reason { get; init; } = "";
}
