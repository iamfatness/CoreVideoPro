using System.Text.Json;
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
    private sealed record EditState(NativeGraphicsDraft Draft, NativeGraphicsDraft Original, long BaseRevision, NativeGraphicsDraft[] Undo);
    private readonly Dictionary<string,EditState> _privateEdits = new(StringComparer.Ordinal);
    private void RememberDraft()
    {
        if (SelectedId.Length != 0 && Draft is not null && _original is not null)
            _privateEdits[SelectedId] = new(Draft,_original,_draftBaseRevision,_undo.ToArray());
    }

    private NativeGraphicsOperationResult? _lastOperation;
    public GraphicsWorkspaceSession(Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> invoke,
        Func<NativeGraphicsState?> observe) => (_invoke, _observe) = (invoke, observe);

    public event EventHandler? Changed;
    public event EventHandler? PreviewChanged;
    public NativeGraphicsState? State { get; private set; }
    public string SelectedId { get; private set; } = "";
    public NativeGraphicsInstance? Selected => State?.Instances.FirstOrDefault(i => i.InstanceId == SelectedId);
    public NativeGraphicsTemplate? Template => State?.Templates.FirstOrDefault(t => t.TemplateId == Draft?.TemplateId && t.TemplateVersion == Draft.TemplateVersion);
    public NativeGraphicsDraft? Draft { get; private set; }
    public NativeGraphicsDraft? Preview => CompareOriginal ? _original : Draft;
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
        foreach (var id in _privateEdits.Keys.Where(id=>!state.Instances.Any(i=>i.InstanceId==id)).ToArray()) _privateEdits.Remove(id);
        if (epochChanged) {
            SelectedId = ""; Draft = _original = null; _undo.Clear(); _gesture = null; _privateEdits.Clear();
            Status = "Native core restarted. Graphics remain hidden; choose a saved cue explicitly.";
            PreviewChanged?.Invoke(this, EventArgs.Empty);
        }
        // Preserve collection selection, focus and local drafts as frame facts advance.
        var signature = state.CoreEpoch + "|" + string.Join('|', state.Instances.Select(i =>
            $"{i.InstanceId}:{i.Revision}:{i.CueRevision}:{i.CuePending}:{i.ProgramRevision}:{i.Preparation}:{i.Visibility}:{i.CurrentlyComposed}:{i.Warning}:{i.CueWarning}:{i.BindingState}:{i.Clock?.State}:{i.Clock?.Text}:{i.ClockConfigurationRevision}")) +
            "|" + string.Join('|', state.Templates.Select(t => $"{t.TemplateId}:{t.TemplateVersion}:{t.ManifestHash}")) +
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
        RememberDraft();
        SelectedId = id; _undo.Clear(); _gesture = null; CompareOriginal = false;
        _draftBaseRevision = item.Revision;
        Draft = From(item, false); _original = From(item, item.ProgramRevision > 0);
        if (_privateEdits.TryGetValue(id,out var saved)) { Draft=saved.Draft; _original=saved.Original; _draftBaseRevision=saved.BaseRevision; _undo.AddRange(saved.Undo); }
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
    public void SetField(string field, JsonElement value)
    {
        if (Draft is null) return;
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
        if (Busy) return;
        RememberDraft(); SelectedId=""; _undo.Clear(); _gesture=null; CompareOriginal=false; _draftBaseRevision=0;
        Draft=_original=NewDraft(template);
        Status="Private library preview — edit this look, then Create hidden cue. Program is unchanged."; NotifyPreview();
    }
    public Task<bool> CreateAsync(NativeGraphicsTemplate template, string scope = "global", string sceneId = "", CancellationToken cancellation = default) =>
        Run(async () => {
            var draft = Selected is null && Draft?.TemplateId == template.TemplateId && Draft.TemplateVersion == template.TemplateVersion ? Draft : NewDraft(template);
            var id = "graphic-" + Guid.NewGuid().ToString("N");
            if (!await Send("graphics.instance.create", new() { ["instanceId"] = id, ["templateId"] = template.TemplateId,
                ["templateVersion"] = template.TemplateVersion, ["scope"] = scope, ["sceneId"] = sceneId, ["fields"] = draft.Fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement }, cancellation)) return false;
            // Selection here is deliberate operator intent, not metadata refresh.
            Busy = false; Select(id); Busy = true; return true;
        });

    public Task<bool> CueAsync(CancellationToken cancellation = default) => Run(async () => {
        if (!await CommitDraft("cue", cancellation)) return false;
        // The commit result identifies the accepted revision. A newer snapshot
        // may already contain another client's edit; never cue that edit.
        return await Send("graphics.instance.cue", new() { ["instanceId"] = SelectedId,
            ["expectedRevision"] = _draftBaseRevision }, cancellation);
    });
    public Task<bool> UpdateProgramAsync(CancellationToken cancellation = default) => Run(() => {
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
    public Task<bool> ClearAsync(CancellationToken cancellation = default) => Run(() => Send("graphics.program.clear", new() { ["channel"] = "all" }, cancellation));
    public Task<bool> RemoveAsync(CancellationToken cancellation = default) => Run(async () => {
        if (Selected is not { ProgramRevision: 0 }) { Status = "Hide the on-air cue before deleting it."; return false; }
        var result = await SelectedCommand("graphics.instance.remove", [], cancellation);
        if (result) { SelectedId = ""; Draft = _original = null; _undo.Clear(); PreviewChanged?.Invoke(this, EventArgs.Empty); }
        return result;
    });
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
    public Task<bool> RefreshCatalogAsync(CancellationToken cancellation = default) => Run(() => Send("graphics.template.list", [], cancellation));
    public Task<bool> RestoreAsync(GraphicsSavedCue cue, CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity? binding = null, CancellationToken cancellation = default) => Run(async () => {
        if (cue.Draft is not { } draft) { Status = "Saved cue has no valid draft."; return false; }
        if (cue.SourceId.Length != 0 && binding is null) { Status = "Choose the current source explicitly before restoring this bound cue."; return false; }
        var id = "graphic-" + Guid.NewGuid().ToString("N");
        var body = new Dictionary<string, object?> { ["instanceId"] = id, ["templateId"] = draft.TemplateId, ["templateVersion"] = draft.TemplateVersion,
            ["scope"] = cue.Scope, ["sceneId"] = cue.SceneId, ["fields"] = draft.Fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement };
        if (binding is not null) body["binding"] = binding;
        if (!await Send("graphics.instance.create", body, cancellation)) return false;
        Busy = false; Select(id); Busy = true;
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
        var fields = selected.Clock is null ? draft.Fields : JsonSerializer.SerializeToElement(draft.Fields.EnumerateObject().Where(p=>p.Name!="time").ToDictionary(p=>p.Name,p=>p.Value.Clone()),Json);
        var accepted = await Send("graphics.instance.update", new() { ["instanceId"] = selected.InstanceId, ["expectedRevision"] = _draftBaseRevision,
            ["target"] = target, ["fields"] = fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement }, cancellation);
        if (accepted && _lastOperation is { CurrentRevision: > 0 } result && result.InstanceId == selected.InstanceId)
            _draftBaseRevision = result.CurrentRevision;
        return accepted;
    }
    private Task<bool> SelectedCommand(string action, Dictionary<string, object?> extra, CancellationToken cancellation)
    {
        if (Selected is not { } selected) { Status = "Choose a cue first."; return Task.FromResult(false); }
        extra["instanceId"] = selected.InstanceId; extra["expectedRevision"] = selected.Revision;
        return Send(action, extra, cancellation);
    }
    private async Task<bool> Send(string action, Dictionary<string, object?> body, CancellationToken cancellation)
    {
        var epoch = State?.CoreEpoch;
        if (string.IsNullOrEmpty(epoch)) { Status = "Native Graphics is unavailable."; return false; }
        body["schemaVersion"] = 1; body["coreEpoch"] = epoch; body["clientId"] = "graphics-workspace";
        body["requestId"] = "operator-" + Guid.NewGuid().ToString("N");
        var result = await _invoke(action, JsonSerializer.Serialize(body, Json), cancellation);
        _lastOperation = result;
        Observe(_observe()); Status = result.Reason; return result.Accepted;
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
        var template = State?.Templates.FirstOrDefault(t => t.TemplateId == item.TemplateId && t.TemplateVersion == item.TemplateVersion);
        if (template?.Runtime == "declarative") {
            var names = template.Fields.Select(f => f.Id).ToHashSet(StringComparer.Ordinal);
            fields = JsonSerializer.SerializeToElement(fields.EnumerateObject().Where(p => names.Contains(p.Name))
                .ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal), Json);
        }
        return new(item.TemplateId, item.TemplateVersion, fields, program ? item.ProgramStyle ?? item.Style : item.Style,
            program ? item.ProgramAnimations ?? item.Animations : item.Animations, program ? item.ProgramPlacement : item.Placement);
    }
    private static bool Same(NativeGraphicsDraft a, NativeGraphicsDraft b) =>
        a.TemplateId == b.TemplateId && a.TemplateVersion == b.TemplateVersion && a.Style == b.Style && a.Animations == b.Animations && a.Placement == b.Placement && JsonElement.DeepEquals(a.Fields, b.Fields);
}
