using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>The operator-facing design steps over <see cref="GraphicsDesignService"/> and the workspace
/// session's private draft (Graphics Designer slice 1): create / open (install, then browse as the
/// draft), Save (a look that is not a design first becomes a new design; the draft then follows the
/// design so a retry never makes a second copy) and Save to playlist. Every outcome is a message; a
/// design that was stored is reported as stored even when the editor could not open it. Moving the
/// editor off a design (open/create another) uninstalls the design's versions nothing references,
/// unless its draft holds unsaved template edits; its typed values, style and placement are remembered
/// (without holding a slot) and come back when it is opened again.</summary>
public sealed class GraphicsDesignWorkflow
{
    private readonly GraphicsDesignService _designs;
    private readonly GraphicsWorkspaceSession _session;
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly Func<NativeGraphicsState?> _snapshot;
    private bool _opening;

    public GraphicsDesignWorkflow(GraphicsDesignService designs, GraphicsWorkspaceSession session, GraphicsWorkspaceLibrary library, Func<NativeGraphicsState?> snapshot) =>
        (_designs, _session, _library, _snapshot) = (designs, session, library, snapshot);

    /// <summary>Raised after a design became the session's private draft.</summary>
    public event EventHandler? Opened;

    public GraphicsDesign? DesignFor(NativeGraphicsDraft? draft) =>
        draft is null ? null : _library.Document.Designs.FirstOrDefault(d => d.TemplateId == draft.TemplateId);

    /// <summary>The design a playlist row shows (by its template id); null for a built-in or imported look.</summary>
    public GraphicsDesign? DesignForRow(GraphicsPlaylistRow row) =>
        GraphicsDesignIds.IsDesignTemplateId(row.TemplateId) ? _library.Document.Designs.FirstOrDefault(d => d.TemplateId == row.TemplateId) : null;

    /// <summary>The editor's browse draft is this design's current version (not a cue, not an older
    /// version a playlist row pins): the Designer can show it as is, unsaved edits included.</summary>
    public bool IsEditing(GraphicsDesign design) =>
        _session.SelectedId.Length == 0 && _session.Draft is { } draft && draft.TemplateId == design.TemplateId && draft.TemplateVersion == design.TemplateVersion;

    /// <summary>Show a design in the Designer: when it is already the private draft (<see cref="IsEditing"/>) it
    /// is shown as is (unsaved edits kept, no core needed; <see cref="Opened"/> is raised), else it is opened.</summary>
    public Task<GraphicsDesignResult> OpenInDesignerAsync(GraphicsDesign design)
    {
        if (!IsEditing(design)) return OpenAsync(design.DesignId);
        Opened?.Invoke(this, EventArgs.Empty);
        return Task.FromResult(new GraphicsDesignResult(true, "", design, _session.Template));
    }

    /// <summary>Operate's "Open in Design" on a row: its design in the Designer (the result says why when it
    /// cannot open); null when the row is not a design's, so the builder opens the row instead.</summary>
    public async Task<GraphicsDesignResult?> OpenForRowAsync(GraphicsPlaylistRow row) =>
        DesignForRow(row) is { } design ? await OpenInDesignerAsync(design) : null;

    public async Task<GraphicsDesignResult> CreateAsync(string? kind, string? lookTemplateId, int lookTemplateVersion, string name)
    {
        var leaving = Leaving();
        var created = kind is { Length: > 0 } ? await _designs.CreateBlankAsync(kind, name)
            : lookTemplateId is { Length: > 0 } ? await _designs.CreateFromAsync(lookTemplateId, lookTemplateVersion, name)
            : new GraphicsDesignResult(false, "Choose a blank kind or a look to start from.", null, null);
        if (!created.Ok || created.Template is not { } template) return created;
        if (!Show(template)) return created with { Message = created.Message + " It is stored; open it from Your designs." };
        await ReleaseLeft(leaving, template);
        return created;
    }

    /// <summary>One open at a time (the builder and Operate can both start one): a second is refused, not run.</summary>
    public async Task<GraphicsDesignResult> OpenAsync(string designId)
    {
        if (_opening) return new(false, "Graphics is busy. Try Open design again in a moment.", null, null);
        _opening = true;
        try {
            var leaving = Leaving();
            var opened = await _designs.OpenAsync(designId);
            if (!opened.Ok || opened.Template is not { } template) return opened;
            if (!Show(template)) return opened with { Ok = false, Message = "Graphics is busy. Try Open design again in a moment." };
            await ReleaseLeft(leaving, template);
            return opened;
        } finally { _opening = false; }
    }

    /// <summary>Delete a design; refused while it is the editor's draft, so the draft never points at
    /// a removed version.</summary>
    public async Task<GraphicsDesignResult> DeleteAsync(string designId)
    {
        await _library.LoadAsync();
        if (DesignFor(_session.Draft)?.DesignId == designId)
            return new(false, "This design is open in the editor. Open another design or look first, then delete it.", null, null);
        return await _designs.DeleteAsync(designId);
    }

    /// <summary>Save the private template edits as the design's next version. A look that is not a
    /// design first becomes the new design "&lt;look&gt; copy"; the draft moves onto that design (keeping
    /// the edits) before its first save, so a failed save is retried into the same design.</summary>
    public async Task<GraphicsDesignResult> SaveAsync()
    {
        if (_session.Busy) return new(false, "Graphics is busy. Try Save again in a moment.", null, null);
        if (_session.Draft is not { Declarative: true } draft || _session.TemplateDocument is not { } document)
            return new(false, "Open a design (or a look to copy) before saving.", null, null);
        var selectedLayer = _session.SelectedLayerId; // the re-open below clears it; Carry restores it
        await _library.LoadAsync();
        var design = DesignFor(draft);
        var prefix = "";
        if (design is null) {
            var created = await _designs.CreateFromAsync(draft.TemplateId, draft.TemplateVersion, CopyName(_session.Template?.Name is { Length: > 0 } look ? look : draft.TemplateId));
            if (!created.Ok || created.Design is null) return created;
            design = created.Design;
            if (created.Template is not { } template || !Show(template))
                return created with { Message = $"Created {design.Name}; your edits were not saved: the editor is busy. Open it from Your designs." };
            if (draft.Scratch is null) return created with { Message = $"Saved as a new design: {design.Name}." };
            // Carry the edits onto the new design's draft: a failed save below leaves them there for a retry.
            document = Stamped(document, design);
            if (_session.Draft is { } moved) _session.Edit(moved with { Scratch = document, Fields = GraphicsTemplateEditor.SyncFields(draft.Fields, document) });
            prefix = $"Created {design.Name}; ";
        } else if (draft.Scratch is null) return new(true, "No design changes to save.", design, _session.Template);
        // The values typed into the draft are not part of the template: carry them onto the saved version.
        var typed = _session.Draft ?? draft;
        var saved = await _designs.SaveAsync(design.DesignId, document);
        if (!saved.Ok || saved.Design is null)
            return saved with { Design = design, Message = prefix.Length > 0 ? prefix + "your edits were not saved: " + saved.Message : saved.Message };
        var reopened = await _designs.OpenAsync(saved.Design.DesignId);
        var message = prefix + saved.Message;
        var current = reopened is { Ok: true, Template: { } latest } && Show(latest) ? latest : null;
        // The saved version supersedes every remembered draft of the design's older versions (Show just
        // remembered the pre-save one): they must not keep an old, perhaps on-air, version from eviction.
        _session.ForgetBrowseEdits(design.TemplateId, saved.Design.TemplateVersion);
        if (current is not null) {
            Carry(typed, current, document, selectedLayer);
            return saved with { Message = message, Template = current };
        }
        return saved with { Message = message + " Open it again from Your designs to keep editing." };
    }

    /// <summary>Save, then add (or re-pin) the design's playlist row.</summary>
    public async Task<GraphicsDesignResult> SaveToPlaylistAsync()
    {
        var saved = await SaveAsync();
        if (!saved.Ok || saved.Design is not { } design) return saved;
        if (saved.Template is not { } template || template.TemplateId != design.TemplateId || template.TemplateVersion != design.TemplateVersion
            || _session.Draft is not { } draft || draft.TemplateId != template.TemplateId || draft.TemplateVersion != template.TemplateVersion)
            return saved with { Ok = false, Message = saved.Message + " Not added to the playlist: open the design again, then Save to playlist." };
        var rows = _library.Document.Playlist;
        var row = PlaylistRow(design, template, draft, rows);
        if (!await _library.SavePlaylistAsync(GraphicsBuilder.Upsert(rows, row)))
            return saved with { Ok = false, Message = saved.Message + " Not added to the playlist: " + _library.Status };
        await _designs.ReleaseAsync(design.TemplateId); // the version the row pinned before is now unused
        return saved with { Message = saved.Message + $" \"{row.Name}\" is in the playlist{(row.Hotkey.Length > 0 ? " on " + row.Hotkey : "")}. Show it from Operate when you want it on air." };
    }

    /// <summary>The F-key Save to playlist will use: the design's existing row's key, else the next free one ("" when all twelve are taken).</summary>
    public static string PlaylistHotkey(GraphicsDesign design, IReadOnlyList<GraphicsPlaylistRow> rows) =>
        rows.FirstOrDefault(r => r.TemplateId == design.TemplateId) is { } existing ? existing.Hotkey : GraphicsBuilder.NextFreeHotkey(rows);

    /// <summary>The design's playlist row: its existing row re-pinned to <paramref name="template"/> (name,
    /// hotkey, timing, motion, names list and clock kept), else a new row on the next free F-key. The kind
    /// follows the template family (full screen saves as a title); a new lower third follows Sources and
    /// plays out after 8 s.</summary>
    public static GraphicsPlaylistRow PlaylistRow(GraphicsDesign design, NativeGraphicsTemplate template, NativeGraphicsDraft draft, IReadOnlyList<GraphicsPlaylistRow> rows)
    {
        var existing = rows.FirstOrDefault(r => r.TemplateId == design.TemplateId);
        var kind = GraphicsBuilder.Kinds.FirstOrDefault(k => k.Family.Length > 0 && k.Family == template.Family) ?? GraphicsBuilder.Kind("title")!;
        var lowerThird = kind.Family == "lower-third";
        var hotkey = PlaylistHotkey(design, rows);
        return GraphicsBuilder.RowFrom(kind, template, draft, existing?.Name ?? design.Name, hotkey, existing?.PlayOutAfterMs ?? (lowerThird ? 8000 : 0),
            existing?.FollowSources ?? lowerThird, rows, existing, existing?.Motion, existing?.Variants, existing?.Clock);
    }

    /// <summary>The design the editor is on, when moving off it may uninstall it: a browse draft (not a
    /// cue) of a design with no unsaved template edits. Unsaved edits keep their version installed.</summary>
    private string? Leaving() =>
        _session.SelectedId.Length == 0 && _session.Draft is { Scratch: null } draft && GraphicsDesignIds.IsDesignTemplateId(draft.TemplateId) ? draft.TemplateId : null;

    private async Task ReleaseLeft(string? leaving, NativeGraphicsTemplate now)
    {
        if (leaving is not null && leaving != now.TemplateId) await _designs.ReleaseAsync(leaving);
    }

    /// <summary>Re-apply the pre-save values, style, placement and motion to the re-opened draft
    /// (variables follow the saved manifest; a removed variable's value is dropped) as its new baseline:
    /// no undo entry, so Undo right after Save never reverts them.</summary>
    private void Carry(NativeGraphicsDraft typed, NativeGraphicsTemplate template, NativeGraphicsScratch saved, string selectedLayer)
    {
        if (_session.Draft is not { } fresh) return;
        var manifest = template.Definition is { } definition ? NativeGraphicsScratch.From(definition) : saved;
        _session.Rebase(fresh with { Fields = GraphicsTemplateEditor.SyncFields(typed.Fields, manifest), Style = typed.Style, Placement = typed.Placement, Animations = typed.Animations }, selectedLayer);
    }

    private bool Show(NativeGraphicsTemplate template)
    {
        // The service polled the snapshot until the version appeared; the session may not have observed it yet.
        _session.Observe(_snapshot());
        _session.Browse(template);
        if (_session.Draft is not { } draft || draft.TemplateId != template.TemplateId || draft.TemplateVersion != template.TemplateVersion) return false;
        Opened?.Invoke(this, EventArgs.Empty);
        return true;
    }

    private static string CopyName(string look)
    {
        var name = look + " copy";
        return name.Length > GraphicsDesignIds.MaxNameLength ? name[..GraphicsDesignIds.MaxNameLength].TrimEnd() : name;
    }

    private static NativeGraphicsScratch Stamped(NativeGraphicsScratch document, GraphicsDesign design)
    {
        var manifest = JsonNode.Parse(document.Manifest)!.AsObject();
        manifest["templateId"] = design.TemplateId; manifest["templateVersion"] = design.TemplateVersion; manifest["name"] = design.Name;
        return NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(manifest));
    }
}

/// <summary>Core-restart reinstall gate: armed by a new core generation, it fires once for the first
/// graphics state of an epoch it has not handled yet.</summary>
public sealed class GraphicsDesignResync
{
    private bool _pending;
    private string _epoch = "";

    /// <summary>Arm on a new core generation; true when the current state can be handled now.</summary>
    public bool Arm(NativeGraphicsState? state) { _pending = true; return Observe(state); }

    /// <summary>True exactly once per armed new epoch with graphics state present.</summary>
    public bool Observe(NativeGraphicsState? state)
    {
        if (!_pending || state is not { CoreEpoch.Length: > 0 } || state.CoreEpoch == _epoch) return false;
        _pending = false; _epoch = state.CoreEpoch;
        return true;
    }
}
