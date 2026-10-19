using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsWorkspaceSession
{
    public IReadOnlyList<string> LastAddedStarterIds { get; private set; } = [];
    public void PreviewStarter(GraphicsStarterGraphic graphic)
    {
        if (Busy || State?.Templates.Any(t => t.TemplateId == graphic.Template.TemplateId && t.TemplateVersion == graphic.Template.TemplateVersion) != true) return;
        EndGesture(); RememberDraft();
        SelectedId = ""; SelectedLayerId = ""; PreviewSample = "current"; CompareOriginal = false;
        _undo.Clear(); _gesture = null; _draftBaseRevision = 0;
        Draft = _original = graphic.Draft;
        Status = "Try the sample privately. Add this show template when you are ready; nothing is on air.";
        NotifyPreview();
    }

    public Task<bool> AddStarterAsync(IReadOnlyList<GraphicsStarterGraphic> graphics, CancellationToken cancellation = default) => Run(async () => {
        LastAddedStarterIds = [];
        if (State is null || graphics.Count != 4 || State.Instances.Count + graphics.Count > 8) {
            Status = "This template needs four free graphic slots. Your current graphics were kept. Hide and remove unused cues, then try again.";
            return false;
        }
        var epoch = State.CoreEpoch;
        if (graphics.Any(g => g.Draft.Scratch is not null)) {
            Status = "Save your template layer edits as a version before adding the show template. Your private edits were kept.";
            return false;
        }
        if (graphics.Any(g => !State.Templates.Any(t => t.TemplateId == g.Template.TemplateId && t.TemplateVersion == g.Template.TemplateVersion && t.ManifestHash == g.Template.ManifestHash))) {
            Status = "A template version is unavailable. Refresh show templates and try again. Your current graphics were kept.";
            return false;
        }
        var added = new List<string>();
        foreach (var graphic in graphics) {
            if (State?.CoreEpoch != epoch) { Status = "The media core reconnected. Add the show template again; nothing was shown."; return false; }
            var draft = graphic.Draft;
            var id = "graphic-" + Guid.NewGuid().ToString("N");
            if (!await Send("graphics.instance.create", new() { ["instanceId"] = id, ["templateId"] = draft.TemplateId,
                ["templateVersion"] = draft.TemplateVersion, ["scope"] = "global", ["fields"] = draft.Fields,
                ["style"] = draft.StyleWire(), ["animations"] = draft.Animations, ["placement"] = draft.Placement, ["image"] = draft.Image }, cancellation)) {
                Status = $"Added {added.Count} of four hidden graphics before the operation stopped: {Status}. Nothing was shown; existing graphics were kept.";
                return false;
            }
            added.Add(id);
            LastAddedStarterIds = added.ToArray();
            var item = State?.Instances.FirstOrDefault(i => i.InstanceId == id);
            if (item is null || !await Send("graphics.instance.update", new() { ["instanceId"] = id, ["expectedRevision"] = item.Revision,
                ["target"] = "cue", ["label"] = graphic.ShowName.Length == 0 ? graphic.Name : graphic.ShowName + " · " + graphic.Name }, cancellation)) {
                Status = $"The new graphic was added hidden, but its label could not be set. Nothing was shown. {Status}";
                return false;
            }
        }
        Busy = false; Select(added[0]); Busy = true;
        Status = "Your four graphics are ready to customize. Choose one, Cue to Preview, then Show on Program when ready. Nothing is on air yet.";
        return true;
    });
}
