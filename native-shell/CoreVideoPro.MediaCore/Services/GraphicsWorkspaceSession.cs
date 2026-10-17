using System.Text.Json;
using System.Text;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Operator-owned private edit session. Commands share the HTTP/OSC
/// boundary; native snapshots alone establish accepted Cue and Program state.
/// Call on one operator dispatcher. No timer, renderer or preference-file writes.</summary>
public sealed class GraphicsWorkspaceSession
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private readonly Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> _invoke;
    private readonly Func<NativeGraphicsState?> _observe;
    private readonly List<NativeGraphicsDraft> _undo = [];
    private NativeGraphicsDraft? _gesture, _original;
    private string _statusSignature = "";
    private long _draftBaseRevision;
    private sealed record EditState(NativeGraphicsDraft Draft, NativeGraphicsDraft Original, long BaseRevision, NativeGraphicsDraft[] Undo,string SelectedLayer="");
    private readonly Dictionary<string,EditState> _privateEdits = new(StringComparer.Ordinal);
    private readonly Dictionary<(string,int),EditState> _browseEdits=[];
    private void RememberDraft()
    {
        if (SelectedId.Length != 0 && Draft is not null && _original is not null)
            _privateEdits[SelectedId] = new(Draft,_original,_draftBaseRevision,_undo.ToArray(),SelectedLayerId);
        else if(Draft is not null && _original is not null) _browseEdits[(Draft.TemplateId,Draft.TemplateVersion)]=new(Draft,_original,0,_undo.ToArray(),SelectedLayerId);
    }

    public GraphicsWorkspaceSession(Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> invoke,
        Func<NativeGraphicsState?> observe) => (_invoke, _observe) = (invoke, observe);

    public event EventHandler? Changed;
    public event EventHandler? PreviewChanged;
    public NativeGraphicsState? State { get; private set; }
    public string SelectedId { get; private set; } = "";
    public string SelectedLayerId { get; private set; } = "";
    public void SelectLayer(string id) { if(Busy || id==SelectedLayerId) return;EndGesture();SelectedLayerId=id;Changed?.Invoke(this,EventArgs.Empty); }
    public NativeGraphicsInstance? Selected => State?.Instances.FirstOrDefault(i => i.InstanceId == SelectedId);
    public NativeGraphicsTemplate? Template => State?.Templates.FirstOrDefault(t => t.TemplateId == Draft?.TemplateId && t.TemplateVersion == Draft.TemplateVersion);
    public NativeGraphicsDraft? Draft { get; private set; }
    public NativeGraphicsDraft? Preview {
        get {
            var preview=CompareOriginal?_original:Draft;
            if(preview is not null && Selected?.Clock is { } clock) {
                var fields=preview.Fields.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());fields["time"]=JsonSerializer.SerializeToElement(clock.Text);
                preview=preview with {Fields=JsonSerializer.SerializeToElement(fields,Json)};
            }
            if(preview is not null && PreviewSample!="current") {
                var template=State?.Templates.FirstOrDefault(t=>t.TemplateId==preview.TemplateId && t.TemplateVersion==preview.TemplateVersion);
                var sampled=GraphicsPreviewSamples.Apply(preview,template,PreviewSample);
                var values=sampled.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());
                preview=preview with {Fields=JsonSerializer.SerializeToElement(values,Json)};
            }
            return preview;
        }
    }
    public NativeGraphicsLayout? AuthoringLayout {
        get {
            if(Preview is not { } preview || State?.Authoring is not {State:"validated",Input:{ } input,Layout:{ } layout}) return null;
            var template=State.Templates.FirstOrDefault(t=>t.TemplateId==preview.TemplateId && t.TemplateVersion==preview.TemplateVersion);
            if(input.TemplateId!=preview.TemplateId || input.TemplateVersion!=preview.TemplateVersion || input.ManifestHash!=(preview.Scratch?.Hash ?? template?.ManifestHash) ||
                input.Fields.ValueKind!=JsonValueKind.Object || !JsonElement.DeepEquals(input.Fields,preview.Fields) ||
                layout.Nodes.Count>64 || layout.Width is <1 or >8192 || layout.Height is <1 or >8192 || !double.IsFinite(layout.Width) || !double.IsFinite(layout.Height)) return null;
            if(input.Style.ValueKind==JsonValueKind.Object && input.Style.TryGetProperty("fontFamily",out var font) && font.GetString()!=preview.Style.FontFamily) return null;
            if(layout.Nodes.Any(n=>new[] {n.X,n.Y,n.Width,n.Height,n.ClipX,n.ClipY,n.ClipWidth,n.ClipHeight}.Any(v=>!double.IsFinite(v)) || n.Width<0 || n.Height<0 || n.ClipWidth<0 || n.ClipHeight<0)) return null;
            return layout;
        }
    }
    public string PreviewSample { get; private set; }="current";
    public void SetPreviewSample(string sample) {if(sample is not ("current" or "short" or "long" or "blank" or "unicode")) return;PreviewSample=sample;NotifyPreview();}
    public bool CompareOriginal { get; private set; }
    public bool Busy { get; private set; }
    public bool ImportBusy { get; private set; }
    public string ImportStatus { get; private set; } = "";
    public bool CanUndo => _undo.Count != 0 || _gesture is not null;
    public bool Dirty => Draft is not null && _original is not null && !Same(Draft, _original);
    public string Status { get; private set; } = "Waiting for native Graphics state.";
    public string PreviewValidation
    {
        get {
            if (Preview is not { } draft) return "Choose a look or cue to preview.";
            if (draft.Fields.ValueKind != JsonValueKind.Object) return "The draft fields are not a valid template object.";
            var template = State?.Templates.FirstOrDefault(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
            if (draft.Declarative && template is null) return "This template version is unavailable. Re-import its exact version before previewing.";
            if (template?.Runtime == "declarative") {
                foreach (var field in template.Fields) {
                    if (!draft.Fields.TryGetProperty(field.Id, out var value) || !GraphicsCommandRequest.ValidField(value,field))
                        return $"{field.Id}: enter a value within this template's type and range.";
                    if (field.Required && value.ValueKind == JsonValueKind.String && value.GetString() == "") return $"{field.Id} is required before previewing or cueing.";
                }
            } else if (!draft.Fields.TryGetProperty("name",out var name) || name.ValueKind != JsonValueKind.String || string.IsNullOrEmpty(name.GetString()) || System.Text.Encoding.UTF8.GetByteCount(name.GetString()!) > 256)
                return "Enter a name of up to 256 UTF-8 bytes to preview this lower third.";
            return "";
        }
    }

    public void Observe(NativeGraphicsState? state)
    {
        if (state is null) return;
        var epochChanged = State is not null && State.CoreEpoch != state.CoreEpoch;
        var clockTextChanged = Selected?.Clock?.Text != state.Instances.FirstOrDefault(i=>i.InstanceId==SelectedId)?.Clock?.Text;
        State = state;
        foreach(var key in _browseEdits.Keys.Where(key=>!state.Templates.Any(t=>t.TemplateId==key.Item1 && t.TemplateVersion==key.Item2)).ToArray()) _browseEdits.Remove(key);
        foreach (var id in _privateEdits.Keys.Where(id=>!state.Instances.Any(i=>i.InstanceId==id)).ToArray()) _privateEdits.Remove(id);
        if (epochChanged) {
            SelectedId = ""; SelectedLayerId=""; Draft = _original = null; _undo.Clear(); _gesture = null; _privateEdits.Clear(); _browseEdits.Clear();
            Status = "Native core restarted. Graphics remain hidden; choose a saved cue explicitly.";
            PreviewChanged?.Invoke(this, EventArgs.Empty);
        }
        // Preserve collection selection, focus and local drafts as frame facts advance.
        var signature = state.CoreEpoch + "|" + (state.Automation is { } automation ? $"{automation.Revision}:{automation.Mode}:{automation.Armed}:{automation.Inhibited}:{automation.ManualHold}:{string.Join(',',automation.HeldInstances)}:{automation.Trace.LastOrDefault()?.Sequence}" : "") + "|" + string.Join('|',state.FamilyAuthorities.Select(p=>p.Key+":"+p.Value)) + "|" + string.Join('|',state.Sources.Select(s=>$"{s.Identity}:{s.DisplayName}:{s.VideoOn}")) + "|" + state.Authoring?.Key + "|" + string.Join('|', state.Instances.Select(i =>
            $"{i.InstanceId}:{i.Label}:{i.RackOrder}:{i.Revision}:{i.CueRevision}:{i.CuePending}:{i.ProgramRevision}:{i.Preparation}:{i.Visibility}:{i.CurrentlyComposed}:{i.Warning}:{i.CueWarning}:{i.BindingState}:{i.Clock?.State}:{i.Clock?.Text}:{i.ClockConfigurationRevision}")) +
            "|" + string.Join('|', state.Templates.Select(t => $"{t.TemplateId}:{t.TemplateVersion}:{t.ManifestHash}:{t.Thumbnail?.State}")) +
            "|" + string.Join('|', state.TemplateInstalls.Select(i => $"{i.ClientId}:{i.RequestId}:{i.State}:{i.Reason}"));
        if (signature == _statusSignature && !epochChanged) return;
        _statusSignature = signature;
        if(clockTextChanged) PreviewChanged?.Invoke(this,EventArgs.Empty);
        Changed?.Invoke(this, EventArgs.Empty);
    }
    public void Select(string id)
    {
        var item = State?.Instances.FirstOrDefault(i => i.InstanceId == id);
        if (item is null || Busy) return;
        EndGesture();RememberDraft();
        SelectedId = id; SelectedLayerId=""; PreviewSample="current"; _undo.Clear(); _gesture = null; CompareOriginal = false;
        _draftBaseRevision = item.Revision;
        Draft = From(item, false); _original = From(item, item.ProgramRevision > 0);
        if (_privateEdits.TryGetValue(id,out var saved)) { Draft=saved.Draft; _original=saved.Original; _draftBaseRevision=saved.BaseRevision; _undo.AddRange(saved.Undo); SelectedLayerId=saved.SelectedLayer; }
        Status = item.ProgramRevision > 0 ? "Editing on-air instance — changes not yet applied." : "Private draft — Cue and Program are unchanged.";
        Changed?.Invoke(this, EventArgs.Empty); PreviewChanged?.Invoke(this, EventArgs.Empty);
    }
    public void Edit(NativeGraphicsDraft value)
    {
        if (Busy || Draft is null || value.TemplateId != Draft.TemplateId || value.TemplateVersion != Draft.TemplateVersion || Same(value, Draft)) return;
        if (_gesture is null) PushUndo(Draft);
        Draft = value; CompareOriginal = false;
        Status = "Private draft — use Cue to Preview or Update on air to apply.";
        Changed?.Invoke(this, EventArgs.Empty); PreviewChanged?.Invoke(this, EventArgs.Empty);
    }
    private NativeGraphicsScratch? _parentDocument;
    private string _parentDocumentHash="";
    public NativeGraphicsScratch? TemplateDocument {
        get {
            if(Draft?.Scratch is { } edited) return edited;
            if(Template is not { Definition: { } definition } template) return null;
            if(_parentDocument is null || _parentDocumentHash!=template.ManifestHash) { _parentDocument=NativeGraphicsScratch.From(definition); _parentDocumentHash=template.ManifestHash; }
            return _parentDocument;
        }
    }
    public void EditTemplate(Func<NativeGraphicsScratch,NativeGraphicsScratch> change)
    {
        if(Busy || Draft is not { Declarative:true } draft || TemplateDocument is not { } document) return;
        try {
            var edited=change(document);
            Edit(draft with { Scratch=edited.Hash==Template?.ManifestHash ? null : edited });
        } catch(Exception ex) when(ex is InvalidDataException or InvalidOperationException or JsonException or ArgumentException) {
            Status=ex.Message; Changed?.Invoke(this,EventArgs.Empty);
        }
    }
    public void SetLayerNumber(string id,string property,double value)=>SetLayerNumbers(id,new Dictionary<string,double> {[property]=value});
    public void SetLayerNumbers(string id,IReadOnlyDictionary<string,double> values)
    {
        if(Busy || Draft is not { } draft || TemplateDocument is not { } document) return;
        if(GraphicsTemplateEditor.Layers(document).FirstOrDefault(l=>l.Id==id)?.Locked==true) {Status="Unlock the layer or its parent before editing.";Changed?.Invoke(this,EventArgs.Empty);return;}
        try {
            var layer=GraphicsTemplateEditor.Layer(document,id);
            var fields=draft.Fields.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());
            foreach(var pair in values) {
                if(!double.IsFinite(pair.Value)) throw new InvalidDataException("Layer values must be finite.");
                if(layer.TryGetProperty(pair.Key,out var current) && current.ValueKind==JsonValueKind.Object && current.TryGetProperty("field",out var field)) {
                    var fieldId=field.GetString()!;var descriptor=Template?.Fields.FirstOrDefault(f=>f.Id==fieldId);
                    var value=Math.Clamp(pair.Value,descriptor?.Minimum ?? -1e9,descriptor?.Maximum ?? 1e9);
                    fields[fieldId]=JsonSerializer.SerializeToElement(value);
                } else document=GraphicsTemplateEditor.Set(document,id,pair.Key,JsonSerializer.SerializeToElement(pair.Value));
            }
            Edit(draft with {Fields=JsonSerializer.SerializeToElement(fields,Json),Scratch=document.Hash==Template?.ManifestHash?null:document});
        } catch(Exception ex) when(ex is InvalidDataException or InvalidOperationException or JsonException or ArgumentException) {Status=ex.Message;Changed?.Invoke(this,EventArgs.Empty);}
    }
    public void SetField(string field, JsonElement value)
    {
        if (Draft is null) return;
        PreviewSample="current";
        var fields = Draft.Fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
        fields[field] = value.Clone(); Edit(Draft with { Fields = JsonSerializer.SerializeToElement(fields, Json) });
    }
    public void SetPlacement(NativeGraphicsPlacement? placement) { if (Draft is not null && (placement is null || placement.IsValid)) Edit(Draft with { Placement = placement }); }
    public void SetStyle(LowerThirdAppearance style) { if (Draft is not null && style.IsValid) Edit(Draft with { Style = style }); }
    public void BeginGesture() { if (!Busy) _gesture ??= Draft; }
    public void EndGesture() { if (_gesture is { } before && Draft is { } after && !Same(before, after)) PushUndo(before); _gesture = null; }
    public void CancelGesture() { if (_gesture is { } before) { Draft = before; _gesture = null; NotifyPreview(); } }
    public void Undo()
    {
        if (Busy) return;
        if (_gesture is not null) { CancelGesture(); return; }
        if (_undo.Count == 0) return;
        Draft = _undo[^1]; _undo.RemoveAt(_undo.Count - 1); CompareOriginal = false; NotifyPreview();
    }
    public void CancelDraft() {
        if (Busy || Selected is not { } selected) return;
        Draft = From(selected,false); _original=From(selected,selected.ProgramRevision>0); _draftBaseRevision=selected.Revision;
        _privateEdits.Remove(selected.InstanceId); _undo.Clear(); _gesture=null; CompareOriginal=false; NotifyPreview();
    }
    public void SetBinding(CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity? binding)
    {
        if(Draft is not { } draft) return;
        if(binding is not null && State?.Sources.Any(s=>s.Identity==binding && s.VideoOn)!=true) {Status="This source generation is unavailable. Choose a current source.";Changed?.Invoke(this,EventArgs.Empty);return;}
        Edit(draft with {Binding=binding});
    }
    public void CopySourceText(string name,string title)
    {
        if(Draft is not { } draft) return;
        var fields=draft.Fields.EnumerateObject().ToDictionary(p=>p.Name,p=>p.Value.Clone());
        var available=Template?.Fields;
        if(!draft.Declarative || available?.Any(f=>f.Id=="name" && f.Type=="string")==true) fields["name"]=JsonSerializer.SerializeToElement(name);
        if(!draft.Declarative || available?.Any(f=>f.Id=="title" && f.Type=="string")==true) fields["title"]=JsonSerializer.SerializeToElement(title);
        Edit(draft with {Fields=JsonSerializer.SerializeToElement(fields,Json)});
    }
    public void SetCompare(bool value) { CompareOriginal = value; NotifyPreview(); }
    private void NotifyPreview() { Changed?.Invoke(this, EventArgs.Empty); PreviewChanged?.Invoke(this, EventArgs.Empty); }
    private void PushUndo(NativeGraphicsDraft value) { if (_undo.Count == 64) _undo.RemoveAt(0); _undo.Add(value); }

    private static NativeGraphicsDraft NewDraft(NativeGraphicsTemplate template)
    {
        var fields = template.Runtime == "declarative" ? template.Fields.ToDictionary(f => f.Id,
            f => f.Default?.Clone() ?? JsonSerializer.SerializeToElement(""), StringComparer.Ordinal) :
            new Dictionary<string,JsonElement> { ["name"] = JsonSerializer.SerializeToElement("Example guest"), ["title"] = JsonSerializer.SerializeToElement("Producer") };
        foreach (var field in template.Fields.Where(f => f.Required && f.Type == "string"))
            if (fields[field.Id].ValueKind == JsonValueKind.String && fields[field.Id].GetString() == "")
                fields[field.Id] = JsonSerializer.SerializeToElement(field.Id == "name" ? "Example guest" : "Title");
        return new(template.TemplateId,template.TemplateVersion,JsonSerializer.SerializeToElement(fields,Json),
            LowerThirdAppearance.FromPreset(template.TemplateId),template.Animations,template.DefaultPlacement);
    }
    public void Browse(NativeGraphicsTemplate template)
    {
        if (Busy || (Selected is null && Draft?.TemplateId==template.TemplateId && Draft.TemplateVersion==template.TemplateVersion)) return;
        EndGesture();RememberDraft(); SelectedId=""; SelectedLayerId=""; PreviewSample="current"; _undo.Clear(); _gesture=null; CompareOriginal=false; _draftBaseRevision=0;
        Draft=_original=NewDraft(template);
        if(_browseEdits.TryGetValue((template.TemplateId,template.TemplateVersion),out var saved)) {Draft=saved.Draft;_original=saved.Original;_undo.AddRange(saved.Undo);SelectedLayerId=saved.SelectedLayer;}
        Status="Private library preview — edit this look, then Create hidden cue. Program is unchanged."; NotifyPreview();
    }
    public Task<bool> CreateAsync(NativeGraphicsTemplate template, string scope = "global", string sceneId = "", CancellationToken cancellation = default) =>
        Run(async () => {
            var draft = Selected is null && Draft?.TemplateId == template.TemplateId && Draft.TemplateVersion == template.TemplateVersion ? Draft : NewDraft(template);
            var id = "graphic-" + Guid.NewGuid().ToString("N");
            if (!await Send("graphics.instance.create", new() { ["instanceId"] = id, ["templateId"] = template.TemplateId,
                ["templateVersion"] = template.TemplateVersion, ["scope"] = scope, ["sceneId"] = sceneId, ["fields"] = draft.Fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement, ["image"] = draft.Image, ["binding"] = draft.Binding }, cancellation)) return false;
            // Selection here is deliberate operator intent, not metadata refresh.
            Busy = false; Select(id); Busy = true; if(draft.Scratch is not null) {Draft=draft;NotifyPreview();} return true;
        });

    public Task<bool> CueAsync(CancellationToken cancellation = default) => Run(async () => {
        if(PreviewSample!="current") {Status="Return to your draft values before cueing; samples never change accepted copy.";return false;}
        if (!await CommitDraft("cue", cancellation)) return false;
        // The commit result identifies the accepted revision. A newer snapshot
        // may already contain another client's edit; never cue that edit.
        return await Send("graphics.instance.cue", new() { ["instanceId"] = SelectedId,
            ["expectedRevision"] = _draftBaseRevision }, cancellation);
    });
    public Task<bool> UpdateProgramAsync(CancellationToken cancellation = default) => Run(() => {
        if(PreviewSample!="current") {Status="Return to your draft values before updating on air.";return Task.FromResult(false);}
        if (Selected is not { ProgramRevision: > 0 }) { Status = "Show a prepared Cue before updating it on air."; return Task.FromResult(false); }
        return CommitDraft("program", cancellation);
    });
    public Task<bool> ShowAsync(CancellationToken cancellation = default) => Run(async () => {
        if (Selected is not { CueRevision: > 0 } selected) { Status = "Cue a prepared revision before Show."; return false; }
        return await Send("graphics.instance.trigger", new() { ["instanceId"] = selected.InstanceId, ["expectedRevision"] = selected.CueRevision,
            ["target"] = "cue", ["action"] = "show" }, cancellation);
    });
    public Task<bool> HideAsync(CancellationToken cancellation = default) => Run(() => SelectedCommand("graphics.instance.trigger", new() { ["action"] = "hide" }, cancellation));
    public Task<bool> ClockAsync(string action,NativeGraphicsClockConfiguration? config=null,CancellationToken cancellation=default,long? expectedClockRevision=null) => Run(() => {
        var extra=new Dictionary<string,object?> { ["action"]=action };
        if(config is not null) { extra["configuration"]=config; extra["expectedClockRevision"]=expectedClockRevision ?? Selected?.ClockConfigurationRevision; }
        return SelectedCommand("graphics.clock.control",extra,cancellation);
    });
    public Task<bool> CueSuggestionAsync(string instanceId,CancellationToken cancellation=default)=>Run(async()=> {
        var suggestion=State?.Automation?.Suggestions.FirstOrDefault(s=>s.InstanceId==instanceId);
        var target=State?.Instances.FirstOrDefault(i=>i.InstanceId==instanceId);
        if(State?.Automation is not {Mode:"suggest",Inhibited:false,ManualHold:false} automation || suggestion?.Binding is not { } binding || target is null ||
            automation.HeldInstances.Contains(instanceId) || State.Sources.All(s=>s.Identity!=binding || !s.VideoOn)) {Status="The suggestion is no longer qualified. Wait for a current rendered subject.";return false;}
        var fields=new Dictionary<string,string> { ["name"]=suggestion.Name,["title"]=suggestion.Secondary };
        var template=State.Templates.FirstOrDefault(t=>t.TemplateId==target.TemplateId && t.TemplateVersion==target.TemplateVersion);
        if(template?.Runtime=="declarative") {
            foreach(var key in fields.Keys.ToArray()) if(!template.Fields.Any(f=>f.Id==key && f.Type=="string" && f.Bindable)) fields.Remove(key);
            if(!fields.ContainsKey("name")) {Status="The target has no bindable name field.";return false;}
        }
        var result=await SendOperation("graphics.instance.update",new() {["instanceId"]=instanceId,["expectedRevision"]=target.Revision,["target"]="cue",["fields"]=fields,["binding"]=binding},cancellation);
        if(!result.Accepted) return false;
        return await Send("graphics.instance.cue",new() {["instanceId"]=instanceId,["expectedRevision"]=result.CurrentRevision>0?result.CurrentRevision:target.Revision},cancellation);
    });
    public Task<bool> RenameCueAsync(string label,CancellationToken cancellation=default)=>Run(()=>SelectedCommand("graphics.instance.update",new() {["target"]="cue",["label"]=label},cancellation));
    public Task<bool> MoveCueAsync(int direction,CancellationToken cancellation=default)=>Run(()=>Selected is { } selected && State is { } state && selected.RackOrder+Math.Sign(direction)>=0 && selected.RackOrder+Math.Sign(direction)<state.Instances.Count
        ?SelectedCommand("graphics.instance.update",new() {["target"]="cue",["rackOrder"]=selected.RackOrder+Math.Sign(direction)},cancellation):Task.FromResult(false));
    public Task<bool> ConfigureAutomationAsync(GraphicsAutomationConfiguration configuration,CancellationToken cancellation=default)=>Run(()=>Send("graphics.automation.configure",new() {["configuration"]=configuration,["expectedAutomationRevision"]=State?.Automation?.Revision ?? 1},cancellation));
    public Task<bool> AutomationControlAsync(string action,string mode="",string instanceId="",CancellationToken cancellation=default)=>Run(()=>Send("graphics.automation.control",new() {["action"]=action,["mode"]=mode,["instanceId"]=instanceId},cancellation));
    public Task<bool> SetAuthorityAsync(string family,string authority,CancellationToken cancellation=default)=>Run(()=>Send("graphics.authority.set",new() {["family"]=family,["authority"]=authority},cancellation));
    public Task<bool> ClearAsync(CancellationToken cancellation = default) => Run(() => Send("graphics.program.clear", new() { ["channel"] = "all" }, cancellation));
    public Task<bool> RemoveAsync(CancellationToken cancellation = default) => Run(async () => {
        if (Selected is not { ProgramRevision: 0 }) { Status = "Hide the on-air cue before deleting it."; return false; }
        var result = await SelectedCommand("graphics.instance.remove", [], cancellation);
        if (result) { SelectedId = ""; Draft = _original = null; _undo.Clear(); PreviewChanged?.Invoke(this, EventArgs.Empty); }
        return result;
    });
    public bool TemplateSaveBusy { get; private set; }
    public async Task<bool> SaveTemplateVersionAsync(CancellationToken cancellation=default)
    {
        if(TemplateSaveBusy || Draft is not { Scratch: { } scratch } before || Template is not { } parent) return false;
        var epoch=State?.CoreEpoch; var selectedId=SelectedId; TemplateSaveBusy=true; Changed?.Invoke(this,EventArgs.Empty);
        try {
            var document=System.Text.Json.Nodes.JsonNode.Parse(scratch.Manifest)!.AsObject();
            var version=checked((State?.Templates.Where(t=>t.TemplateId==parent.TemplateId).Select(t=>t.TemplateVersion).DefaultIfEmpty(parent.TemplateVersion).Max() ?? parent.TemplateVersion)+1);
            document["templateVersion"]=version;
            if(!await Send("graphics.template.install",new() { ["manifest"]=document.ToJsonString(),["packageRoot"]=parent.PackageRoot },cancellation)) return false;
            var until=DateTime.UtcNow.AddSeconds(5); NativeGraphicsTemplate? installed=null;
            do {
                await Task.Delay(100,cancellation); await Send("graphics.template.list",[],cancellation);
                if(State?.CoreEpoch!=epoch) {Status="Native core restarted. Template save did not change Program.";return false;}
                installed=State?.Templates.FirstOrDefault(t=>t.TemplateId==parent.TemplateId && t.TemplateVersion==version);
            } while(installed is null && DateTime.UtcNow<until);
            if(installed is null) {Status="The template version is not installed. Check import diagnostics; the previous version and Program are retained.";return false;}
            if(installed.Definition is not { } actual || !JsonElement.DeepEquals(JsonSerializer.SerializeToElement(document),actual)) {
                Status="Another import owns that template version. Your private edits and Program were retained. Save again as a new version.";return false;
            }
            if(SelectedId==selectedId && Draft?.Scratch?.Hash==scratch.Hash && Draft.TemplateId==before.TemplateId && Draft.TemplateVersion==before.TemplateVersion) {
                Draft=Draft with { TemplateVersion=version,Scratch=null }; _gesture=null; CompareOriginal=false;
                Status="Template version saved. Cue or Update explicitly adopts it; the on-air version is unchanged."; NotifyPreview();
            } else Status="Template version saved. Your newer private edits were retained.";
            return true;
        } catch(Exception ex) when(ex is not OutOfMemoryException) {Status=ex.Message;return false;}
        finally {TemplateSaveBusy=false;Changed?.Invoke(this,EventArgs.Empty);}
    }
    public Task<bool> ImportAsync(string manifest, CancellationToken cancellation = default, string packageRoot = "") => Run(() => Send("graphics.template.install", new() { ["manifest"] = manifest, ["packageRoot"] = packageRoot }, cancellation));
    public async Task<bool> ImportPackageAsync(string path, CancellationToken cancellation = default)
    {
        if (ImportBusy) return false;
        ImportBusy = true; ImportStatus = "Validating local graphics package. Live graphic controls remain available."; Changed?.Invoke(this, EventArgs.Empty);
        try {
            var result = await Send("graphics.template.install", new() { ["packagePath"] = path }, cancellation);
            ImportStatus = Status; return result;
        }
        catch (Exception ex) when (ex is not OutOfMemoryException) { ImportStatus = ex.Message; return false; }
        finally { ImportBusy = false; Changed?.Invoke(this, EventArgs.Empty); }
    }
    public async Task<NativeGraphicsImageReference?> ImportImageAsync(string path,CancellationToken cancellation=default)
    {
        if(ImportBusy) return null;ImportBusy=true;ImportStatus="Verifying local image package. Program is unchanged.";Changed?.Invoke(this,EventArgs.Empty);
        var epoch=State?.CoreEpoch;
        try {
            var root=Environment.GetEnvironmentVariable("COREVIDEO_GRAPHICS_ASSET_ROOT") ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"CoreVideoPro","graphics");
            var staged=await GraphicsImageImport.StageAsync(path,root,cancellation);
            if(State?.CoreEpoch!=epoch) {ImportStatus="Native core restarted; image import cancelled.";return null;}
            if(!await Send("graphics.template.install",new() {["manifest"]=staged.Package.Manifest,["packageRoot"]=staged.Package.PackageRoot},cancellation)) {ImportStatus=Status;return null;}
            var until=DateTime.UtcNow.AddSeconds(5);
            do {
                await Task.Delay(100,cancellation);await Send("graphics.template.list",[],cancellation);
                if(State?.CoreEpoch!=epoch) {ImportStatus="Native core restarted; no image was applied.";return null;}
                if(State?.Templates.Any(t=>t.TemplateId==staged.Reference.TemplateId && t.TemplateVersion==staged.Reference.TemplateVersion)==true) {
                    ImportStatus="Verified image package ready. Applying it to a private draft remains explicit.";return staged.Reference;
                }
            } while(DateTime.UtcNow<until);
            ImportStatus="Native image validation did not install the package. Check import diagnostics; previous look retained.";return null;
        } catch(Exception ex) when(ex is not OutOfMemoryException) {ImportStatus=ex.Message;return null;}
        finally {ImportBusy=false;Changed?.Invoke(this,EventArgs.Empty);}
    }
    public Task<bool> RefreshCatalogAsync(CancellationToken cancellation = default) => Run(() => Send("graphics.template.list", new() {["includeThumbnails"]=true}, cancellation));
    public Task<bool> RestoreAsync(GraphicsSavedCue cue, CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity? binding = null, CancellationToken cancellation = default) => Run(async () => {
        if (cue.Draft is not { } draft) { Status = "Saved cue has no valid draft."; return false; }
        if (cue.SourceId.Length != 0 && binding is null) { Status = "Choose the current source explicitly before restoring this bound cue."; return false; }
        var id = "graphic-" + Guid.NewGuid().ToString("N");
        var body = new Dictionary<string, object?> { ["instanceId"] = id, ["templateId"] = draft.TemplateId, ["templateVersion"] = draft.TemplateVersion,
            ["scope"] = cue.Scope, ["sceneId"] = cue.SceneId, ["fields"] = draft.Fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement, ["image"] = draft.Image };
        if (binding is not null) body["binding"] = binding;
        if (!await Send("graphics.instance.create", body, cancellation)) return false;
        Busy = false; Select(id); Busy = true;
        Draft=draft with {Binding=binding};NotifyPreview();
        if(cue.Name.Length>0 && !await SelectedCommand("graphics.instance.update",new() {["target"]="cue",["label"]=cue.Name},cancellation)) return false;
        if (cue.ClockConfiguration is { } clock && !await SelectedCommand("graphics.clock.control",new() { ["action"]="configure",["configuration"]=clock,["expectedClockRevision"]=Selected?.ClockConfigurationRevision },cancellation)) return false;
        Status = "Restored hidden. Cue and Show remain explicit; automation is unarmed."; return true;
    });
    public Task<bool> DuplicateAsync(CancellationToken cancellation = default)
    {
        if (Selected is not { } selected || Draft is not { } draft) return Task.FromResult(false);
        return RestoreAsync(new() { Id = selected.InstanceId, Draft = draft, Scope = selected.Scope, SceneId = selected.SceneId, SourceId = selected.SourceId, ClockConfiguration=selected.Clock is { } clock ? clock.Configuration with { RestoreRemainingSeconds=clock.Configuration.Mode=="countdown" ? clock.RemainingSeconds : null } : null }, selected.Binding, cancellation);
    }

    private async Task<bool> CommitDraft(string target, CancellationToken cancellation)
    {
        if (Draft is not { } draft || Selected is not { } selected) { Status = "Choose a cue first."; return false; }
        if(draft.Scratch is not null) { Status="Save the edited template as an immutable version before Cue or Update."; return false; }
        var fields = selected.Clock is null ? draft.Fields : JsonSerializer.SerializeToElement(draft.Fields.EnumerateObject().Where(p=>p.Name!="time").ToDictionary(p=>p.Name,p=>p.Value.Clone()),Json);
        var result = await SendOperation("graphics.instance.update", new() { ["instanceId"] = selected.InstanceId, ["expectedRevision"] = _draftBaseRevision,
            ["target"] = target, ["templateVersion"] = draft.TemplateVersion, ["fields"] = fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement, ["image"] = draft.Image, ["binding"] = draft.Binding }, cancellation);
        if (result is {Accepted:true,CurrentRevision:>0} && result.InstanceId == selected.InstanceId)
            _draftBaseRevision = result.CurrentRevision;
        return result.Accepted;
    }
    private Task<bool> SelectedCommand(string action, Dictionary<string, object?> extra, CancellationToken cancellation)
    {
        if (Selected is not { } selected) { Status = "Choose a cue first."; return Task.FromResult(false); }
        extra["instanceId"] = selected.InstanceId; extra["expectedRevision"] = selected.Revision;
        return Send(action, extra, cancellation);
    }
    private async Task<bool> Send(string action,Dictionary<string,object?> body,CancellationToken cancellation)=>(await SendOperation(action,body,cancellation)).Accepted;
    private async Task<NativeGraphicsOperationResult> SendOperation(string action, Dictionary<string, object?> body, CancellationToken cancellation)
    {
        var epoch = State?.CoreEpoch;
        if (string.IsNullOrEmpty(epoch)) { Status = "Native Graphics is unavailable."; return new() {Reason=Status}; }
        body["schemaVersion"] = 1; body["coreEpoch"] = epoch; body["clientId"] = "graphics-workspace";
        body["requestId"] = "operator-" + Guid.NewGuid().ToString("N");
        var result = await _invoke(action, JsonSerializer.Serialize(body, Json), cancellation);
        Observe(_observe());
        if(State?.CoreEpoch!=epoch) result=new() {RequestId=result.RequestId,Reason="Native core restarted. The old operation was not replayed."};
        Status = result.Reason; return result;
    }
    private async Task<bool> Run(Func<Task<bool>> action)
    {
        if (Busy) return false;
        Busy = true; Changed?.Invoke(this, EventArgs.Empty);
        try { return await action(); }
        catch (Exception ex) when (ex is not OutOfMemoryException) { Status = ex.Message; return false; }
        finally { Busy = false; Changed?.Invoke(this, EventArgs.Empty); }
    }
    private NativeGraphicsDraft From(NativeGraphicsInstance item, bool program)
    {
        var fields = JsonSerializer.SerializeToElement(program ? item.ProgramFields ?? item.Fields : item.Fields, Json);
        var template = State?.Templates.FirstOrDefault(t => t.TemplateId == item.TemplateId && t.TemplateVersion == (program && item.ProgramTemplateVersion>0 ? item.ProgramTemplateVersion : item.TemplateVersion));
        if (template?.Runtime == "declarative") {
            var names = template.Fields.Select(f => f.Id).ToHashSet(StringComparer.Ordinal);
            fields = JsonSerializer.SerializeToElement(fields.EnumerateObject().Where(p => names.Contains(p.Name))
                .ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal), Json);
        }
        return new(item.TemplateId, program && item.ProgramTemplateVersion > 0 ? item.ProgramTemplateVersion : item.TemplateVersion, fields, program ? item.ProgramStyle ?? item.Style : item.Style,
            program ? item.ProgramAnimations ?? item.Animations : item.Animations, program ? item.ProgramPlacement : item.Placement) {Binding=program ? item.ProgramBinding : item.Binding,Image=program?item.ProgramImage:item.Image};
    }
    private static bool Same(NativeGraphicsDraft a, NativeGraphicsDraft b) =>
        a.Image == b.Image && a.Binding == b.Binding && a.TemplateId == b.TemplateId && a.TemplateVersion == b.TemplateVersion && a.Style == b.Style && a.Animations == b.Animations && a.Placement == b.Placement && a.Scratch?.Hash==b.Scratch?.Hash && JsonElement.DeepEquals(a.Fields, b.Fields);
}
