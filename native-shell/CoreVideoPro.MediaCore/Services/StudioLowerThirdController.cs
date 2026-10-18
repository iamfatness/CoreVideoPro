using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record StudioLowerThirdSubject(SourceInstanceIdentity Identity, string Name, string Title);

/// <summary>Studio's existing Show intent consumes a pinned graphics draft,
/// never the editor selection. Loading/assignment has no native side effects.</summary>
public sealed class StudioLowerThirdController
{
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> _invoke;
    private readonly Func<NativeGraphicsState?> _observe;
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private string _epoch = "", _instanceId = "";
    private bool _busy;
    public bool Busy => _busy;
    public GraphicsStudioLowerThird? Assignment => _library.Document.StudioLowerThird;
    public string Label {
        get {
            if (Assignment is not { } assigned) return "Not assigned";
            var pin = _library.Document.Templates.FirstOrDefault(t => t.TemplateId == assigned.Cue.Draft?.TemplateId && t.TemplateVersion == assigned.Cue.Draft?.TemplateVersion);
            return pin?.Definition is { } definition && definition.TryGetProperty("name", out var name) && name.ValueKind == JsonValueKind.String
                ? name.GetString() ?? assigned.Cue.Name : assigned.Cue.Name;
        }
    }
    public string Detail => Assignment is { } assigned
        ? assigned.Cue.Name + (assigned.FollowProgramSource ? " · Program source text" : " · Cue text")
        : "Choose a lower-third cue in Graphics and use Use in Studio.";
    public event EventHandler? Changed;

    public StudioLowerThirdController(GraphicsWorkspaceLibrary library,
        Func<string, string, CancellationToken, Task<NativeGraphicsOperationResult>> invoke,
        Func<NativeGraphicsState?> observe)
    {
        (_library, _invoke, _observe) = (library, invoke, observe);
        library.Changed += (_, _) => Changed?.Invoke(this, EventArgs.Empty);
    }

    public async Task<NativeGraphicsOperationResult> ShowAsync(string sceneId,
        Func<string?, StudioLowerThirdSubject?> resolveSubject, CancellationToken cancellation = default)
    {
        if (_busy) return Refuse("A Studio lower-third operation is already in progress.");
        _busy = true; Changed?.Invoke(this, EventArgs.Empty);
        try {
            await _library.LoadAsync();
            var assignment = Assignment;
            if (assignment?.Cue.Draft is not { } draft) return Refuse("Assign a lower-third look in Graphics first.");
            if (assignment.Cue.Scope == "scene" && assignment.Cue.SceneId != sceneId)
                return Refuse("The assigned lower third belongs to a different scene. Choose an applicable look in Graphics.");
            if (_observe() is not { } state || state.CoreEpoch.Length == 0) return Refuse("Native Graphics is unavailable.");
            var epoch = state.CoreEpoch;
            var required = new HashSet<(string Id, int Version)>();
            if (draft.Declarative) required.Add((draft.TemplateId, draft.TemplateVersion));
            if (draft.Image is { } image) required.Add((image.TemplateId, image.TemplateVersion));
            foreach (var key in required) {
                var pin = _library.Document.Templates.FirstOrDefault(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
                if (pin is null) return Refuse("The assigned template or image pin is unavailable. Reassign a verified look.");
                var installed = state.Templates.FirstOrDefault(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
                if (installed is null) {
                    var imported = await Send("graphics.template.install", epoch, new() { ["manifest"] = pin.Definition.GetRawText(), ["packageRoot"] = pin.PackageRoot }, cancellation);
                    if (!imported.Accepted) return imported;
                    var limit = DateTime.UtcNow.AddSeconds(5);
                    do {
                        await Task.Delay(50, cancellation);
                        var listed = await Send("graphics.template.list", epoch, [], cancellation);
                        if (!listed.Accepted) return listed;
                        state = Current(epoch);
                        installed = state.Templates.FirstOrDefault(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
                    } while (installed is null && DateTime.UtcNow < limit);
                }
                if (installed?.ManifestHash != pin.ManifestHash)
                    return Refuse("The assigned template is missing or differs from its saved pin. Program is unchanged.");
            }
            state = Current(epoch);
            if (state.Templates.FirstOrDefault(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion)?.Family != "lower-third")
                return Refuse("The assigned template is not an available lower third.");
            StudioLowerThirdSubject? subject = null;
            if (assignment.FollowProgramSource || assignment.Cue.SourceId.Length > 0) {
                subject = resolveSubject(assignment.FollowProgramSource ? null : assignment.Cue.SourceId);
                if (subject is null || !state.Sources.Any(s => s.VideoOn && s.Identity == subject.Identity))
                    return Refuse("The assigned subject is unavailable. No stale source generation was reused.");
            }
            var fields = draft.Fields;
            if (assignment.FollowProgramSource && subject is not null) {
                var template = state.Templates.First(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
                var values = fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
                if (template.Runtime == "declarative" && !template.Fields.Any(f => f.Id == "name" && f.Type == "string"))
                    return Refuse("This look has no standard name field. Assign it with cue text instead.");
                values["name"] = JsonSerializer.SerializeToElement(subject.Name);
                if (template.Runtime != "declarative" || template.Fields.Any(f => f.Id == "title" && f.Type == "string"))
                    values["title"] = JsonSerializer.SerializeToElement(subject.Title);
                fields = JsonSerializer.SerializeToElement(values, Json);
            }
            // Only retire our previous hidden transport instance. Never evict a
            // user cue or remove anything still on Program to make room.
            if (_epoch == epoch && state.Instances.FirstOrDefault(i => i.InstanceId == _instanceId) is { } old) {
                if (old.ProgramRevision > 0 || old.ShowPending) return Refuse("Hide the current lower third before showing a new assigned look.");
                var removed = await Send("graphics.instance.remove", epoch, new() { ["instanceId"] = old.InstanceId, ["expectedRevision"] = old.Revision }, cancellation);
                if (!removed.Accepted) return removed;
            }
            _epoch = epoch; _instanceId = "studio-" + Guid.NewGuid().ToString("N");
            var created = await Send("graphics.instance.create", epoch, new() {
                ["instanceId"] = _instanceId, ["templateId"] = draft.TemplateId, ["templateVersion"] = draft.TemplateVersion,
                ["scope"] = assignment.Cue.Scope, ["sceneId"] = assignment.Cue.SceneId,
                ["fields"] = fields, ["style"] = draft.StyleWire(), ["animations"] = draft.Animations,
                ["placement"] = draft.Placement, ["image"] = draft.Image, ["binding"] = subject?.Identity
            }, cancellation);
            if (!created.Accepted) return created;
            var revision = created.CurrentRevision;
            if (revision < 1 || created.InstanceId != _instanceId) return Refuse("The native create result has no matching revision identity.");
            var cued = await Send("graphics.instance.cue", epoch, new() { ["instanceId"] = _instanceId, ["expectedRevision"] = revision }, cancellation);
            if (!cued.Accepted) return cued;
            var until = DateTime.UtcNow.AddSeconds(5);
            while (true) {
                var current = Current(epoch).Instances.FirstOrDefault(i => i.InstanceId == _instanceId);
                if (current is null || current.Revision != revision) return Refuse("The prepared Studio look changed. Retry Show explicitly.");
                if (current.CueRevision == revision && !current.CuePending) break;
                if (DateTime.UtcNow >= until) return Refuse("The assigned look is still preparing. Retry Show when ready.");
                await Task.Delay(50, cancellation);
            }
            return await Send("graphics.instance.trigger", epoch, new() { ["instanceId"] = _instanceId, ["expectedRevision"] = revision, ["target"] = "cue", ["action"] = "show" }, cancellation);
        }
        catch (InvalidOperationException ex) { return Refuse(ex.Message); }
        catch (OperationCanceledException) { return Refuse("Studio Show cancelled. Retry explicitly when ready."); }
        finally { _busy = false; Changed?.Invoke(this, EventArgs.Empty); }
    }

    private NativeGraphicsState Current(string epoch) => _observe() is { } state && state.CoreEpoch == epoch
        ? state : throw new InvalidOperationException("Native core changed. Studio Show cancelled; no automatic replay.");
    private Task<NativeGraphicsOperationResult> Send(string action, string epoch, Dictionary<string, object?> body, CancellationToken cancellation)
    {
        Current(epoch);
        body["schemaVersion"] = 1; body["clientId"] = "studio-lower-third";
        body["requestId"] = "studio-" + Guid.NewGuid().ToString("N"); body["coreEpoch"] = epoch;
        return _invoke(action, JsonSerializer.Serialize(body, Json), cancellation);
    }
    private static NativeGraphicsOperationResult Refuse(string reason) => new() { Reason = reason };
}
