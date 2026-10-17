using CoreVideoPro.MediaCore.Contracts;

namespace CoreVideoPro.MediaCore.Models;
public sealed record GraphicsAutomationRule
{
    public string Id {get;init;}="guest-introduction";
    public string InstanceId {get;init;}="";
    public bool Enabled {get;init;}=true;
    public string SceneId {get;init;}="";
    public string SourceId {get;init;}="";
    public double DwellMs {get;init;}=1500;
    public double DurationMs {get;init;}=8000;
    public double CooldownMs {get;init;}
    public bool SuppressScreenShare {get;init;}=true;
    [System.Text.Json.Serialization.JsonIgnore]
    public bool IsValid => Id.Length is >0 and <=64 && Id.All(c=>char.IsAsciiLetterLower(c)||char.IsAsciiDigit(c)||c=='-') && InstanceId.Length is >0 and <=64 && InstanceId.All(c=>char.IsAsciiLetterLower(c)||char.IsAsciiDigit(c)||c=='-') && SceneId.Length<=256 && SourceId.Length<=256 &&
        double.IsFinite(DwellMs)&&DwellMs is >=0 and <=60000 && double.IsFinite(DurationMs)&&DurationMs is >=0 and <=3600000 && double.IsFinite(CooldownMs)&&CooldownMs is >=0 and <=3600000;
}
public sealed record GraphicsAutomationSourceText
{
    public SourceInstanceIdentity? Binding {get;init;}
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)] public string? Name {get;init;}
    [System.Text.Json.Serialization.JsonIgnore(Condition=System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)] public string? Secondary {get;init;}
}
public sealed record GraphicsAutomationConfiguration
{
    public IReadOnlyList<GraphicsAutomationRule> Rules {get;init;}=[];
    public IReadOnlyDictionary<string,GraphicsAutomationSourceText> SourceTexts {get;init;}=new Dictionary<string,GraphicsAutomationSourceText>();
}
public sealed record NativeGraphicsAutomation
{
    public long DroppedHistory {get;init;}
    public IReadOnlyList<NativeGraphicsSuggestion> Suggestions {get;init;}=[];
    public long ActivationRevision {get;init;}=1;
    public long Revision {get;init;}=1;
    public string Mode {get;init;}="manual";
    public bool Armed {get;init;}
    public bool Inhibited {get;init;}
    public bool ManualHold {get;init;}
    public IReadOnlyList<string> HeldInstances {get;init;}=[];
    public GraphicsAutomationConfiguration Configuration {get;init;}=new();
    public IReadOnlyList<GraphicsAutomationTrace> Trace {get;init;}=[];
}
public sealed record GraphicsAutomationTrace
{
    public long Sequence {get;init;}
    public string RuleId {get;init;}="";
    public string InstanceId {get;init;}="";
    public string State {get;init;}="";
    public string Reason {get;init;}="";
    public string Subject {get;init;}="";
    public long Revision {get;init;}
}

public sealed record NativeGraphicsSuggestion
{
    public string RuleId {get;init;}="";
    public string InstanceId {get;init;}="";
    public string Name {get;init;}="";
    public string Secondary {get;init;}="";
    public SourceInstanceIdentity? Binding {get;init;}
}
