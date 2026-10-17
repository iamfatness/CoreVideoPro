using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Saved workspace consumer. Loading has no native side effects. Only
/// explicit restore imports a pinned definition and creates a hidden instance.</summary>
public sealed class GraphicsWorkspaceLibrary
{
    private readonly GraphicsDocumentStore _store;
    private readonly GraphicsWorkspaceSession _session;
    private Task? _load;
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
    public async Task<bool> SaveCueAsync(string name)
    {
        await LoadAsync();
        if (_session.Draft is not { } draft || _session.Selected is not { } selected) return Fail("Select a cue before saving.");
        if (string.IsNullOrWhiteSpace(name) || name.Length > 128) return Fail("Use a cue name of 1–128 characters.");
        var cues = Document.Cues.ToList(); var index = cues.FindIndex(c => c.Id == selected.InstanceId);
        var saved = new GraphicsSavedCue { Id = selected.InstanceId, Name = name.Trim(), Scope = selected.Scope, SceneId = selected.SceneId, SourceId = selected.SourceId, Draft = draft, ClockConfiguration=selected.Clock is { } clock ? clock.Configuration with { RestoreRemainingSeconds=clock.Configuration.Mode=="countdown" ? clock.RemainingSeconds : null } : null };
        if (index < 0) cues.Add(saved); else cues[index] = saved;
        var templates = Document.Templates.ToList();
        if (draft.Declarative) {
            var definition = _session.State?.Templates.FirstOrDefault(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
            if (definition?.Definition is not { } document) return Fail("The exact native template document is unavailable. The previous saved cue is retained.");
            var previous = templates.FindIndex(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
            var pin = new GraphicsSavedTemplate(definition.TemplateId, definition.TemplateVersion, definition.ManifestHash, document.Clone(), definition.PackageRoot);
            if (previous < 0) templates.Add(pin);
            else if (templates[previous].ManifestHash != definition.ManifestHash) return Fail("Saved template version conflicts with the current native definition. Import a new version explicitly.");
            else templates[previous] = pin;
        }
        return await Save(Document with { Cues = cues.ToArray(), Templates = templates.ToArray() }, "Cue and template pin saved. Program is unchanged.");
    }
    public async Task<bool> SaveStyleAsync(string name)
    {
        await LoadAsync();
        if (_session.Draft is not { } draft || string.IsNullOrWhiteSpace(name) || name.Length > 80) return Fail("Select a cue and use a style name of 1–80 characters.");
        var styles = Document.StylePresets.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal); styles[name.Trim()] = draft.Style;
        return await Save(Document with { StylePresets = styles }, "Style preset saved. Draft, Cue and Program are unchanged.");
    }
    public void ApplyStyle(string name)
    {
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
        var epoch = _session.State?.CoreEpoch;
        var pin = Document.Templates.FirstOrDefault(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
        if (draft.Declarative && pin is null) return Fail("The pinned template is missing. Import its exact version before restoring.");
        var installed = _session.State?.Templates.FirstOrDefault(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
        if (installed is null && pin is not null) {
            if (!await _session.ImportAsync(pin.Definition.GetRawText(), cancellation, pin.PackageRoot)) return Fail(_session.Status);
            var limit = DateTime.UtcNow.AddSeconds(5);
            do {
                await Task.Delay(100, cancellation); await _session.RefreshCatalogAsync(cancellation);
                installed = _session.State?.Templates.FirstOrDefault(t => t.TemplateId == draft.TemplateId && t.TemplateVersion == draft.TemplateVersion);
                if (_session.State?.CoreEpoch != epoch) return Fail("Native core restarted. Restore cancelled; nothing was shown.");
            } while (installed is null && DateTime.UtcNow < limit);
        }
        if (installed is null) return Fail("The native template is not ready. Check the import result and retry Restore.");
        if (pin is not null && pin.ManifestHash != installed.ManifestHash) return Fail("Installed template differs from the saved version pin. Restore refused; Program is unchanged.");
        var restored = await _session.RestoreAsync(cue, binding, cancellation); Status = _session.Status; Changed?.Invoke(this, EventArgs.Empty); return restored;
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
