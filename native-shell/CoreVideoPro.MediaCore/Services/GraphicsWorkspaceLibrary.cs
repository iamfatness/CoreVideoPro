using CoreVideoPro.MediaCore.Models;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Saved workspace consumer. Loading has no native side effects. Only
/// explicit restore imports a pinned definition and creates a hidden instance.</summary>
public sealed partial class GraphicsWorkspaceLibrary
{
    private readonly GraphicsDocumentStore _store;
    private readonly GraphicsWorkspaceSession _session;
    private Task? _load;
    private readonly Dictionary<string,string> _restoredCueIds=new(StringComparer.Ordinal);
    private string _restoredEpoch="";
    public GraphicsShowDocument Document { get; private set; } = new();
    public bool ReadOnly { get; private set; }
    public bool Busy { get; private set; }
    public string Status { get; private set; } = "Graphics library has not been loaded.";
    public event EventHandler? Changed;
    public GraphicsWorkspaceLibrary(GraphicsDocumentStore store, GraphicsWorkspaceSession session) => (_store, _session) = (store, session);
    public Task LoadAsync() => _load ??= LoadCore();
    private async Task LoadCore()
    {
        var result = await _store.LoadAsync(); Document = result.Document; ReadOnly = result.ReadOnly; Status = result.Status; Changed?.Invoke(this, EventArgs.Empty);
    }
    public async Task<bool> SaveCueAsync(string name,JsonElement? sourceOverrides=null,JsonElement? brand=null)
    {
        await LoadAsync();
        if (_session.Draft is not { } draft || _session.Selected is not { } selected) return Fail("Select a cue before saving.");
        if (string.IsNullOrWhiteSpace(name) || name.Length > 128) return Fail("Use a cue name of 1–128 characters.");
        var cues = Document.Cues.ToList(); var index = cues.FindIndex(c => c.Id == selected.InstanceId);
        var saved = new GraphicsSavedCue { Id = selected.InstanceId, Name = name.Trim(), Scope = selected.Scope, SceneId = selected.SceneId, SourceId = draft.Binding?.SourceId ?? "", Draft = draft, ClockConfiguration=selected.Clock is { } clock ? clock.Configuration with { RestoreRemainingSeconds=clock.Configuration.Mode=="countdown" ? clock.RemainingSeconds : null } : null };
        if (index < 0) cues.Add(saved); else cues[index] = saved;
        var templates = Document.Templates.ToList();
        var pins=new HashSet<(string Id,int Version)>();
        if(draft.Declarative) pins.Add((draft.TemplateId,draft.TemplateVersion));
        if(draft.Image is { } image) pins.Add((image.TemplateId,image.TemplateVersion));
        foreach(var key in pins) {
            var definition=_session.State?.Templates.FirstOrDefault(t=>t.TemplateId==key.Id && t.TemplateVersion==key.Version);
            if(definition?.Definition is not { } document) return Fail("The exact native template or image package is unavailable. Previous saved cue retained.");
            var previous=templates.FindIndex(t=>t.TemplateId==key.Id && t.TemplateVersion==key.Version);
            var pin=new GraphicsSavedTemplate(key.Id,key.Version,definition.ManifestHash,document.Clone(),definition.PackageRoot);
            if(previous<0) templates.Add(pin);
            else if(templates[previous].ManifestHash!=definition.ManifestHash) return Fail("The saved template pin conflicts with the native version. Import a new version explicitly.");
            else templates[previous]=pin;
        }
        return await Save(Document with { Cues = cues.ToArray(), Templates = templates.ToArray(),Authority=_session.State?.FamilyAuthorities.GetValueOrDefault("lower-third")=="instances"?"volt":Document.Authority,SourceOverrides=Merge(Document.SourceOverrides,sourceOverrides),Brand=Merge(Document.Brand,brand) }, "Cue and template pin saved. Program is unchanged.");
    }
    private static JsonElement? Merge(JsonElement? previous,JsonElement? current)
    {
        if(current is not {ValueKind:JsonValueKind.Object} fresh) return previous;
        var values=previous is {ValueKind:JsonValueKind.Object} old?old.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone(),StringComparer.Ordinal):new Dictionary<string,JsonElement>(StringComparer.Ordinal);
        foreach(var value in fresh.EnumerateObject()) values[value.Name]=value.Value.Clone();return JsonSerializer.SerializeToElement(values);
    }
    public async Task<bool> ImportLegacyAsync(NativeGraphicsDraft draft,string sourceId,JsonElement sourceOverrides,JsonElement brand,
        IReadOnlyDictionary<string,LowerThirdAppearance> styles,string logoReference="")
    {
        await LoadAsync();
        if(draft.Declarative || !draft.Style.IsValid || sourceOverrides.ValueKind!=JsonValueKind.Object || brand.ValueKind!=JsonValueKind.Object)
            return Fail("The legacy graphics snapshot is invalid. Original preferences retained.");
        if(Document.Cues.Any(c=>c.Id=="legacy-lower-third")) return Fail("The existing legacy import is retained. Duplicate or edit its saved copy instead of overwriting it.");
        var metadata=new Dictionary<string,JsonElement>(StringComparer.Ordinal) { ["legacyImageReference"]=JsonSerializer.SerializeToElement(logoReference),["migration"]=JsonSerializer.SerializeToElement("copied-hidden-v1") };
        var cue=new GraphicsSavedCue {Id="legacy-lower-third",Name="Existing lower third",SourceId=sourceId,Draft=draft,Metadata=metadata};
        var presets=Document.StylePresets.ToDictionary(p=>p.Key,p=>p.Value,StringComparer.Ordinal);
        foreach(var pair in styles) {if(!presets.ContainsKey(pair.Key)) presets[pair.Key]=pair.Value;}
        var pins=Document.Templates.ToList();
        if(draft.Image is { } logo) {
            var installed=_session.State?.Templates.FirstOrDefault(t=>t.TemplateId==logo.TemplateId && t.TemplateVersion==logo.TemplateVersion);
            if(installed?.Definition is not { } definition) return Fail("The verified logo package is not installed. Original legacy look retained.");
            if(!pins.Any(p=>p.TemplateId==logo.TemplateId && p.TemplateVersion==logo.TemplateVersion)) pins.Add(new(logo.TemplateId,logo.TemplateVersion,installed.ManifestHash,definition.Clone(),installed.PackageRoot));
        }
        return await Save(Document with {Cues=[..Document.Cues,cue],Templates=pins,StylePresets=presets,SourceOverrides=Document.SourceOverrides ?? sourceOverrides.Clone(),Brand=Document.Brand ?? brand.Clone()},
            "Existing text, appearance, timings, styles and source overrides copied to a hidden saved cue. Original preferences and Program are unchanged. Restore, inspect, Cue and Show explicitly.");
    }
    public async Task<bool> SaveAutomationRulesAsync()
    {
        await LoadAsync();
        if(_session.State?.Automation is not { } automation) return Fail("Native automation is unavailable.");
        var rules=automation.Configuration.Rules;
        if(rules.Any(r=>!Document.Cues.Any(c=>c.Id==r.InstanceId))) return Fail("Save each rule's target cue before saving its rules. Previous saved rules retained.");
        return await Save(Document with {AutomationRules=rules.ToArray()},"Rule configuration saved. Activation, source generations and holds are never restored.");
    }
    public async Task<bool> RestoreAutomationRulesAsync(Func<IReadOnlyList<GraphicsAutomationRule>,GraphicsAutomationConfiguration> qualify)
    {
        await LoadAsync();var state=_session.State;
        if(state is null) return Fail("Native graphics is unavailable.");
        var rules=new List<GraphicsAutomationRule>();
        foreach(var rule in Document.AutomationRules) {
            var id=state.Instances.Any(i=>i.InstanceId==rule.InstanceId)?rule.InstanceId:
                _restoredEpoch==state.CoreEpoch?_restoredCueIds.GetValueOrDefault(rule.InstanceId):null;
            if(id is null || !state.Instances.Any(i=>i.InstanceId==id)) return Fail("Restore every saved rule target as a hidden cue in this core session before restoring rules. Nothing was armed.");
            rules.Add(rule with {InstanceId=id});
        }
        var restored=await _session.ConfigureAutomationAsync(qualify(rules));
        Status=restored?"Rules restored with current source identities. Mode and Arm remain explicit.":_session.Status;Changed?.Invoke(this,EventArgs.Empty);return restored;
    }
    public async Task<bool> SaveStyleAsync(string name)
    {
        await LoadAsync();
        if (_session.Draft is not { } draft || string.IsNullOrWhiteSpace(name) || name.Length > 80) return Fail("Select a cue and use a style name of 1–80 characters.");
        if(draft.Declarative) {
            if(_session.Template is not { } template) return Fail("The installed template is unavailable.");
            var presets=Document.TemplateStyles.ToDictionary(p=>p.Key,p=>p.Value,StringComparer.Ordinal);
            presets[name.Trim()]=GraphicsTemplateStyle.Capture(draft,template);
            return await Save(Document with {TemplateStyles=presets},"Template style saved without name, title, clock or image copy. Program is unchanged.");
        }
        var styles = Document.StylePresets.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal); styles[name.Trim()] = draft.Style;
        return await Save(Document with { StylePresets = styles }, "Style preset saved. Draft, Cue and Program are unchanged.");
    }
    public void ApplyStyle(string name)
    {
        if(_session.Draft is {Declarative:true} draft && Document.TemplateStyles.TryGetValue(name,out var saved)) {
            if(_session.Template is not { } template || !GraphicsTemplateStyle.TryApply(draft,template,saved,out var edited)) {Fail("This style is incompatible with the selected template version. Your draft is retained.");return;}
            _session.Edit(edited);Status="Template style loaded privately. Copy and intentional blanks are retained.";Changed?.Invoke(this,EventArgs.Empty);return;
        }
        if (Document.StylePresets.TryGetValue(name, out var style)) { _session.SetStyle(style); Status = "Style loaded into the private draft. Cue or Update remains explicit."; Changed?.Invoke(this, EventArgs.Empty); }
    }
    public async Task<bool> RemoveSavedAsync(string id) {
        await LoadAsync();
        return await Save(Document with { Cues = Document.Cues.Where(c => c.Id != id).ToArray() }, "Removed the saved cue only. Native output is unchanged.");
    }
    public async Task<bool> MoveSavedAsync(string id, int direction)
    {
        await LoadAsync();
        var cues = Document.Cues.ToList(); var index = cues.FindIndex(c => c.Id == id); var target = index + Math.Sign(direction);
        if (index < 0 || target < 0 || target >= cues.Count) return false;
        (cues[index], cues[target]) = (cues[target], cues[index]);
        return await Save(Document with { Cues = cues.ToArray() }, "Saved cue order updated. Native output is unchanged.");
    }
    public async Task<bool> RestoreAsync(GraphicsSavedCue cue, CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity? binding = null, CancellationToken cancellation = default)
    {
        await LoadAsync();
        if (cue.Draft is not { } draft) return Fail("The saved cue needs repair.");
        if(cue.Metadata?.TryGetValue("legacyImageReference",out var image)==true && image.ValueKind==JsonValueKind.String && !string.IsNullOrEmpty(image.GetString()) && draft.Image is null)
            return Fail("This legacy cue retains a logo reference that needs verified package import before managed restore. The existing legacy look is retained; no logo was silently dropped.");
        var epoch=_session.State?.CoreEpoch;
        var required=new HashSet<(string Id,int Version)>();
        if(draft.Declarative) required.Add((draft.TemplateId,draft.TemplateVersion));
        if(draft.Image is { } imageReference) required.Add((imageReference.TemplateId,imageReference.TemplateVersion));
        foreach(var key in required) {
            var pin=Document.Templates.FirstOrDefault(t=>t.TemplateId==key.Id && t.TemplateVersion==key.Version);
            if(pin is null) return Fail("The pinned template or logo package is missing. Import its exact version before restoring.");
            var installed=_session.State?.Templates.FirstOrDefault(t=>t.TemplateId==key.Id && t.TemplateVersion==key.Version);
            if(installed is null) {
                if(!await _session.ImportAsync(pin.Definition.GetRawText(),cancellation,pin.PackageRoot)) return Fail(_session.Status);
                var limit=DateTime.UtcNow.AddSeconds(5);
                do {
                    await Task.Delay(100,cancellation);await _session.RefreshCatalogAsync(cancellation);
                    installed=_session.State?.Templates.FirstOrDefault(t=>t.TemplateId==key.Id && t.TemplateVersion==key.Version);
                    if(_session.State?.CoreEpoch!=epoch) return Fail("Native core restarted. Restore cancelled; nothing was shown.");
                } while(installed is null && DateTime.UtcNow<limit);
            }
            if(installed is null) return Fail("The native template or logo is not ready. Check import diagnostics and retry Restore.");
            if(pin.ManifestHash!=installed.ManifestHash) return Fail("Installed template differs from the saved version pin. Program is unchanged.");
        }
        if(!draft.Declarative && !_session.State!.Templates.Any(t=>t.TemplateId==draft.TemplateId && t.TemplateVersion==draft.TemplateVersion)) return Fail("The compatibility look is unavailable.");
        var restored = await _session.RestoreAsync(cue, binding, cancellation);
        if(restored && _session.Selected is { } selected) {
            if(_restoredEpoch!=_session.State?.CoreEpoch) {_restoredCueIds.Clear();_restoredEpoch=_session.State?.CoreEpoch ?? "";}
            _restoredCueIds[cue.Id]=selected.InstanceId;
        }
        Status = _session.Status; Changed?.Invoke(this, EventArgs.Empty); return restored;
    }
    private async Task<bool> Save(GraphicsShowDocument document, string success)
    {
        await LoadAsync(); if (ReadOnly) return Fail("This graphics library is read-only. Original retained.");
        if (Busy) return Fail("A graphics save is in progress. Wait for completion before another edit.");
        Busy = true; Changed?.Invoke(this, EventArgs.Empty);
        try { await _store.SaveAsync(document); Document = document; Status = success; Changed?.Invoke(this, EventArgs.Empty); return true; }
        catch (Exception ex) when (ex is IOException or InvalidDataException or UnauthorizedAccessException or InvalidOperationException) { return Fail(ex.Message); }
        finally { Busy = false; Changed?.Invoke(this, EventArgs.Empty); }
    }
    private bool Fail(string reason) { Status = reason; Changed?.Invoke(this, EventArgs.Empty); return false; }
}
