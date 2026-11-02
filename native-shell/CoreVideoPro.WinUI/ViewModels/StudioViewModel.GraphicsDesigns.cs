using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Graphics Designer slice 1: the show's designs. One <see cref="GraphicsDesignService"/>
/// serves every surface (builder list, start dialog, workspace Save). Opening a design installs its
/// current version and browses it as the workspace session's PRIVATE draft; nothing reaches Preview or
/// Program until the operator shows a playlist row from Operate.</summary>
public sealed partial class StudioViewModel
{
    private GraphicsDesignService? _graphicsDesigns;
    private bool _graphicsDesignsResyncPending;
    private string _graphicsDesignsEpoch = "";

    public GraphicsDesignService GraphicsDesigns => _graphicsDesigns ??= new(GraphicsLibrary, InvokeGraphicsActionAsync, () => _bridge.LastSnapshot?.Graphics);

    /// <summary>Raised after a design became the workspace's private draft (Design → Advanced editor).</summary>
    public event EventHandler? GraphicsDesignOpened;

    /// <summary>The design a draft belongs to (by template id), or null for a built-in or package look.</summary>
    public GraphicsDesign? GraphicsDesignFor(NativeGraphicsDraft? draft) =>
        draft is null ? null : GraphicsLibrary.Document.Designs.FirstOrDefault(d => d.TemplateId == draft.TemplateId);

    /// <summary>Start blank from a seed kind, or copy a look (design or installed template), then open it.</summary>
    public async Task<GraphicsDesignResult> CreateGraphicsDesignAsync(string? kind, string? lookTemplateId, int lookTemplateVersion, string name)
    {
        var created = kind is { Length: > 0 }
            ? await GraphicsDesigns.CreateBlankAsync(kind, name)
            : lookTemplateId is { Length: > 0 } ? await GraphicsDesigns.CreateFromAsync(lookTemplateId, lookTemplateVersion, name)
            : new GraphicsDesignResult(false, "Choose a blank kind or a look to start from.", null, null);
        return created is { Ok: true, Template: { } template } ? ShowGraphicsDesign(created, template) : created;
    }

    public async Task<GraphicsDesignResult> OpenGraphicsDesignAsync(string designId)
    {
        var opened = await GraphicsDesigns.OpenAsync(designId);
        return opened is { Ok: true, Template: { } template } ? ShowGraphicsDesign(opened, template) : opened;
    }

    public Task<GraphicsDesignResult> DuplicateGraphicsDesignAsync(GraphicsDesign design)
    {
        var name = design.Name + " copy";
        return GraphicsDesigns.DuplicateAsync(design.DesignId, name.Length > GraphicsDesignIds.MaxNameLength ? name[..GraphicsDesignIds.MaxNameLength].TrimEnd() : name);
    }

    public Task<GraphicsDesignResult> DeleteGraphicsDesignAsync(string designId) => GraphicsDesigns.DeleteAsync(designId);

    /// <summary>Save the workspace's private template edits as the design's next version. A draft that
    /// is not a design (built-in or package look) first becomes a new design "&lt;look&gt; copy" — looks are
    /// never edited in place. After a save the draft is re-opened on the new version.</summary>
    public async Task<GraphicsDesignResult> SaveGraphicsDesignAsync()
    {
        var session = GraphicsWorkspace;
        if (session.Busy) return new(false, "Graphics is busy. Try Save again in a moment.", null, null);
        if (session.Draft is not { Declarative: true } draft || session.TemplateDocument is not { } document)
            return new(false, "Open a design (or a look to copy) before saving.", null, null);
        await GraphicsLibrary.LoadAsync();
        var design = GraphicsDesignFor(draft);
        if (design is null) {
            var copyName = (session.Template?.Name is { Length: > 0 } lookName ? lookName : draft.TemplateId) + " copy";
            if (copyName.Length > GraphicsDesignIds.MaxNameLength) copyName = copyName[..GraphicsDesignIds.MaxNameLength].TrimEnd();
            var created = await GraphicsDesigns.CreateFromAsync(draft.TemplateId, draft.TemplateVersion, copyName);
            if (!created.Ok || created.Design is null) return created;
            design = created.Design;
            if (draft.Scratch is null) return created.Template is { } template ? ShowGraphicsDesign(created with { Message = $"Saved as a new design: {design.Name}." }, template) : created;
            document = Renamed(document, design.Name);
        } else if (draft.Scratch is null) return new(true, "No design changes to save.", design, session.Template);
        var saved = await GraphicsDesigns.SaveAsync(design.DesignId, document);
        if (!saved.Ok || saved.Design is null) return saved;
        var reopened = await GraphicsDesigns.OpenAsync(saved.Design.DesignId);
        return reopened is { Ok: true, Template: { } current } ? ShowGraphicsDesign(saved with { Template = current }, current)
            : saved with { Message = saved.Message + " " + reopened.Message };
    }

    /// <summary>Save, then add (or update) one playlist row pinned to the design's current version,
    /// on the next free F-key when the design has no row yet.</summary>
    public async Task<GraphicsDesignResult> SaveGraphicsDesignToPlaylistAsync()
    {
        var saved = await SaveGraphicsDesignAsync();
        if (!saved.Ok || saved.Design is not { } design) return saved;
        var session = GraphicsWorkspace;
        var template = saved.Template is { } t && t.TemplateId == design.TemplateId && t.TemplateVersion == design.TemplateVersion ? t : null;
        if (template is null) {
            var opened = await OpenGraphicsDesignAsync(design.DesignId);
            if (!opened.Ok || opened.Template is null) return opened;
            template = opened.Template;
        }
        if (session.Draft is not { } draft || draft.TemplateId != template.TemplateId || draft.TemplateVersion != template.TemplateVersion)
            return new(false, "The design changed while saving. Open it again, then Save to playlist.", design, template);
        var rows = GraphicsPlayout.Playlist;
        var existing = rows.FirstOrDefault(r => r.TemplateId == design.TemplateId);
        var kind = GraphicsBuilder.Kinds.FirstOrDefault(k => k.Family.Length > 0 && k.Family == template.Family) ?? GraphicsBuilder.Kind("title")!;
        var lowerThird = kind.Family == "lower-third";
        var hotkey = existing?.Hotkey is { Length: > 0 } key ? key : GraphicsBuilder.NextFreeHotkey(rows);
        var row = GraphicsBuilder.RowFrom(kind, template, draft, existing?.Name ?? design.Name, hotkey, existing?.PlayOutAfterMs ?? (lowerThird ? 8000 : 0),
            existing?.FollowSources ?? lowerThird, rows, existing, existing?.Motion, existing?.Variants, existing?.Clock);
        if (!await GraphicsLibrary.SavePlaylistAsync(GraphicsBuilder.Upsert(rows, row))) return new(false, GraphicsLibrary.Status, design, template);
        return new(true, $"\"{row.Name}\" is in the playlist{(row.Hotkey.Length > 0 ? " on " + row.Hotkey : "")}. Show it from Operate when you want it on air.", design, template);
    }

    private GraphicsDesignResult ShowGraphicsDesign(GraphicsDesignResult result, NativeGraphicsTemplate template)
    {
        var session = GraphicsWorkspace;
        // The service polled the bridge snapshot until the version appeared; the session may not have
        // observed that snapshot yet, and Browse needs the template in its state.
        session.Observe(_bridge.LastSnapshot?.Graphics);
        session.Browse(template);
        if (session.Draft is not { } draft || draft.TemplateId != template.TemplateId || draft.TemplateVersion != template.TemplateVersion)
            return result with { Ok = false, Message = result.Message + " The editor is busy; open the design again from Your designs." };
        GraphicsMode = GraphicsModeDesign; GraphicsDesignAdvanced = true;
        GraphicsDesignOpened?.Invoke(this, EventArgs.Empty);
        return result;
    }

    private static NativeGraphicsScratch Renamed(NativeGraphicsScratch document, string name)
    {
        var manifest = JsonNode.Parse(document.Manifest)!.AsObject(); manifest["name"] = name;
        return NativeGraphicsScratch.From(System.Text.Json.JsonSerializer.SerializeToElement(manifest));
    }

    // ---- core restart -----------------------------------------------------------------------
    /// <summary>Called from the core-generation hook (OnBridgeProfileChanged): the native registry is
    /// in memory, so design versions the playlist or saved cues pin must be installed again once the
    /// new core's graphics state arrives.</summary>
    private void ArmGraphicsDesignResync()
    {
        _graphicsDesignsResyncPending = true;
        ResyncGraphicsDesigns(_bridge.LastSnapshot?.Graphics);
    }

    private void ResyncGraphicsDesigns(NativeGraphicsState? state)
    {
        if (!_graphicsDesignsResyncPending || state is not { CoreEpoch.Length: > 0 } || state.CoreEpoch == _graphicsDesignsEpoch) return;
        _graphicsDesignsResyncPending = false; _graphicsDesignsEpoch = state.CoreEpoch;
        _ = EnsureGraphicsDesignsInstalledAsync();
    }

    private async Task EnsureGraphicsDesignsInstalledAsync()
    {
        try {
            await GraphicsDesigns.EnsureInstalledForPlaylistAsync();
            if (!GraphicsDesigns.Status.StartsWith("Designs in use are installed", StringComparison.Ordinal)) CommandStatus = "Graphics designs: " + GraphicsDesigns.Status;
        } catch (Exception ex) when (ex is not OutOfMemoryException) { CommandStatus = "Graphics designs could not be reinstalled: " + ex.Message; }
    }
}
