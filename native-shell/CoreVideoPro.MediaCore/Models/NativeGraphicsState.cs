using CoreVideoPro.MediaCore.Contracts;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

/// <summary>Native accepted graphics revisions; no shell-derived on-air state.</summary>
public sealed record NativeGraphicsState
{
    public IReadOnlyDictionary<string,string> FamilyAuthorities {get;init;}=new Dictionary<string,string>();
    public NativeGraphicsAuthoring? Authoring { get; init; }
    public NativeGraphicsAutomation? Automation {get;init;}
    public int SchemaVersion { get; init; }
    public string CoreEpoch { get; init; } = "";
    public IReadOnlyList<NativeGraphicsInstance> Instances { get; init; } = [];
    public IReadOnlyList<NativeGraphicsOperationResult> Operations {get;init;}=[];
    public NativeGraphicsOperationResult? LastResult { get; init; }
    public IReadOnlyList<NativeGraphicsSource> Sources { get; init; } = [];
    public string SourceBindingStatus { get; init; } = "unavailable";
    public NativeGraphicsResourceBudget? ResourceBudgetAtStatePublication { get; init; }
    public IReadOnlyList<NativeGraphicsTemplate> Templates { get; init; } = [];
    public IReadOnlyList<NativeGraphicsTemplateInstall> TemplateInstalls { get; init; } = [];
    public long EncodedTemplateAssetBytes { get; init; }
    public long EncodedTemplateAssetLimit { get; init; }
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
    public NativeGraphicsThumbnail? Thumbnail { get; init; }
    public int SchemaVersion { get; init; }
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public string Name { get; init; } = "";
    public string Family { get; init; } = "";
    public string Runtime { get; init; } = "";
    public string ManifestHash { get; init; } = "";
    public JsonElement? Definition { get; init; }
    public string PackageRoot { get; init; } = "";
    public JsonElement? Canvas { get; init; }
    public JsonElement? Bounds { get; init; }
    public NativeGraphicsPlacement? DefaultPlacement { get; init; }
    public IReadOnlyList<NativeGraphicsTemplateField> Fields { get; init; } = [];
    public JsonElement? Layers { get; init; }
    public IReadOnlyList<string> BrandTokens { get; init; } = [];
    public NativeGraphicsAnimations Animations { get; init; } = new();
    [JsonExtensionData] public Dictionary<string, JsonElement>? ExtensionData { get; init; }
}
public sealed record NativeGraphicsThumbnail
{
    public string State { get; init; }="not-requested";
    public string Reason { get; init; }="";
    public string Backend { get; init; }="";
    public string Appearance { get; init; }="";
    public int Width { get; init; }
    public int Height { get; init; }
    public string PngBase64 { get; init; }="";
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
    public long ResidentResources {get;init;}
    public long ResidentHighWater {get;init;}
    public long ResidentLimit {get;init;}
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
    public string Label {get;init;}="";
    public int RackOrder {get;init;}
    public GraphicsRevisionIdentity? Identity { get; init; }
    public GraphicsPreparationFact? PreparationObservation { get; init; }
    public GraphicsCompositionFact? FirstCompositionObservation { get; init; }
    public string InstanceId { get; init; } = "";
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public NativeGraphicsImageReference? Image {get;init;}
    public NativeGraphicsImageReference? CueImage {get;init;}
    public NativeGraphicsImageReference? ProgramImage {get;init;}
    public NativeGraphicsClock? Clock { get; init; }
    public long ClockConfigurationRevision { get; init; }
    public NativeGraphicsClockConfiguration? ClockRequestedConfiguration { get; init; }
    public string Scope { get; init; } = "";
    public string SceneId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public SourceInstanceIdentity? Binding { get; init; }
    public SourceInstanceIdentity? ProgramBinding { get; init; }
    public string BindingState { get; init; } = "unbound";
    public long Revision { get; init; }
    public long ProgramRevision { get; init; }
    public long CueRevision { get; init; }
    public int CueTemplateVersion { get; init; }
    public int ProgramTemplateVersion { get; init; }
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
    public bool CurrentlyComposed { get; init; }
    public string Warning { get; init; } = "";
    public NativeGraphicsFields Fields { get; init; } = new();
    public LowerThirdAppearance Style { get; init; } = new();
    public NativeGraphicsAnimations Animations { get; init; } = new();
    public NativeGraphicsPlacement? Placement { get; init; }
    public NativeGraphicsPlacement? ProgramPlacement { get; init; }
    public NativeGraphicsPlacement? CuePlacement { get; init; }
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
    public string ClientId {get;init;}="";
    public string InstanceId { get; init; } = "";
    public long CurrentRevision { get; init; }
    public string RequestId { get; init; } = "";
    public bool Accepted { get; init; }
    public string Reason { get; init; } = "";
}

public sealed record NativeGraphicsClock
{
    public string Text { get; init; } = "";
    public string State { get; init; } = "";
    public string FinishAction { get; init; } = "";
    public double RemainingSeconds { get; init; }
    public NativeGraphicsClockConfiguration Configuration { get; init; } = new();
}
public sealed record NativeGraphicsClockConfiguration
{
    public string Mode { get; init; } = "wall";
    public string Zone { get; init; } = "local";
    public string Format { get; init; } = "hms";
    public string Finish { get; init; } = "hold-zero";
    public int OffsetMinutes { get; init; }
    public int DurationSeconds { get; init; } = 300;
    public double? RestoreRemainingSeconds { get; init; }
    [JsonIgnore] public bool IsValid => Mode is "wall" or "countdown" && Zone is "local" or "utc" or "fixed" && Format is "hms" or "hm" &&
        Finish is "hold-zero" or "hide" or "suggest-next" && OffsetMinutes is >=-840 and <=840 && DurationSeconds is >=1 and <=86400 && (RestoreRemainingSeconds is null || double.IsFinite(RestoreRemainingSeconds.Value) && RestoreRemainingSeconds >= 0 && RestoreRemainingSeconds <= DurationSeconds);
}

public sealed record NativeGraphicsAuthoring
{
    public string Key { get; init; } = "";
    public string State { get; init; } = "";
    public string Reason { get; init; } = "";
    public NativeGraphicsAuthoringInput? Input { get; init; }
    public NativeGraphicsLayout? Layout { get; init; }
}
public sealed record NativeGraphicsAuthoringInput
{
    public string TemplateId { get; init; } = "";
    public int TemplateVersion { get; init; }
    public string ManifestHash { get; init; } = "";
    public JsonElement Fields { get; init; }
    public JsonElement Style { get; init; }
}
public sealed record NativeGraphicsLayout
{
    public double Width { get; init; }
    public double Height { get; init; }
    public IReadOnlyList<NativeGraphicsNodeGeometry> Nodes { get; init; } = [];
}
public sealed record NativeGraphicsNodeGeometry
{
    public string Id { get; init; } = "";
    public string Parent { get; init; } = "";
    public string Type { get; init; } = "";
    public double X { get; init; }
    public double Y { get; init; }
    public double Width { get; init; }
    public double Height { get; init; }
    public double ClipX { get; init; }
    public double ClipY { get; init; }
    public double ClipWidth { get; init; }
    public double ClipHeight { get; init; }
    public bool Visible { get; init; }
}
