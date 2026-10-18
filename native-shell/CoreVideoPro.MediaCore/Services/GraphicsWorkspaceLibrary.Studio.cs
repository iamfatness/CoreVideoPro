using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsWorkspaceLibrary
{
    /// <summary>Persist an independent immutable draft. No Cue, Show, authority
    /// change or live source generation is sent by assignment.</summary>
    public async Task<bool> AssignStudioLowerThirdAsync(bool followProgramSource)
    {
        var draft = _session.Draft;
        var selected = _session.Selected;
        var template = _session.Template;
        if (_session.Busy || draft is null || selected is null ||
            template?.Family != "lower-third" || _session.PreviewSample != "current")
            return Fail("Select a lower-third cue and return to Your draft before using it in Studio.");
        if (draft.Scratch is not null || _session.PreviewValidation.Length != 0)
            return Fail("Save a valid immutable template version before assigning this look to Studio.");

        // Capture before awaiting disk I/O: changing selection never changes the
        // snapshot whose assignment the operator actually requested.
        var cue = new GraphicsSavedCue {
            Id = "studio-" + Guid.NewGuid().ToString("N"),
            Name = string.IsNullOrWhiteSpace(selected.Label) ? template.Name : selected.Label,
            Scope = selected.Scope, SceneId = selected.SceneId,
            SourceId = draft.Binding?.SourceId ?? "",
            Draft = draft with { Binding = null }
        };
        var state = _session.State;
        await LoadAsync();
        var templates = Document.Templates.ToList();
        var required = new HashSet<(string Id, int Version)>();
        if (draft.Declarative) required.Add((draft.TemplateId, draft.TemplateVersion));
        if (draft.Image is { } image) required.Add((image.TemplateId, image.TemplateVersion));
        foreach (var key in required) {
            var installed = state?.Templates.FirstOrDefault(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
            if (installed?.Definition is not { } definition)
                return Fail("The exact template or image package is unavailable. Previous Studio assignment retained.");
            var index = templates.FindIndex(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
            if (index >= 0 && templates[index].ManifestHash != installed.ManifestHash)
                return Fail("The saved template pin conflicts with this version. Previous Studio assignment retained.");
            var pin = new GraphicsSavedTemplate(key.Id, key.Version, installed.ManifestHash, definition.Clone(), installed.PackageRoot);
            if (index < 0) templates.Add(pin); else templates[index] = pin;
        }
        return await Save(Document with {
            StudioLowerThird = new(cue, followProgramSource), Templates = templates
        }, "Look assigned to Studio. Current Program is unchanged; the next Show uses this pinned look.");
    }

    public async Task<bool> ClearStudioLowerThirdAsync()
    {
        await LoadAsync();
        return await Save(Document with { StudioLowerThird = null }, "Studio assignment removed. Current Program is unchanged.");
    }
}
