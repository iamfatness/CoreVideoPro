using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>The operator-facing design steps over <see cref="GraphicsDesignService"/> and the workspace
/// session's private draft (Graphics Designer slice 1): create / open (install, then browse as the
/// draft), Save (a look that is not a design first becomes a new design; the draft then follows the
/// design so a retry never makes a second copy) and Save to playlist. Every outcome is a message; a
/// design that was stored is reported as stored even when the editor could not open it.</summary>
public sealed class GraphicsDesignWorkflow
{
    private readonly GraphicsDesignService _designs;
    private readonly GraphicsWorkspaceSession _session;
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly Func<NativeGraphicsState?> _snapshot;

    public GraphicsDesignWorkflow(GraphicsDesignService designs, GraphicsWorkspaceSession session, GraphicsWorkspaceLibrary library, Func<NativeGraphicsState?> snapshot) =>
        (_designs, _session, _library, _snapshot) = (designs, session, library, snapshot);

    /// <summary>Raised after a design became the session's private draft.</summary>
    public event EventHandler? Opened;

    public GraphicsDesign? DesignFor(NativeGraphicsDraft? draft) =>
        draft is null ? null : _library.Document.Designs.FirstOrDefault(d => d.TemplateId == draft.TemplateId);

    public async Task<GraphicsDesignResult> CreateAsync(string? kind, string? lookTemplateId, int lookTemplateVersion, string name)
    {
        var created = kind is { Length: > 0 } ? await _designs.CreateBlankAsync(kind, name)
            : lookTemplateId is { Length: > 0 } ? await _designs.CreateFromAsync(lookTemplateId, lookTemplateVersion, name)
            : new GraphicsDesignResult(false, "Choose a blank kind or a look to start from.", null, null);
        if (!created.Ok || created.Template is not { } template) return created;
        return Show(template) ? created : created with { Message = created.Message + " It is stored; open it from Your designs." };
    }

    public async Task<GraphicsDesignResult> OpenAsync(string designId)
    {
        var opened = await _designs.OpenAsync(designId);
        if (!opened.Ok || opened.Template is not { } template) return opened;
        return Show(template) ? opened : opened with { Ok = false, Message = "Graphics is busy. Try Open design again in a moment." };
    }

    /// <summary>Save the private template edits as the design's next version. A look that is not a
    /// design first becomes the new design "&lt;look&gt; copy"; the draft moves onto that design (keeping
    /// the edits) before its first save, so a failed save is retried into the same design.</summary>
    public async Task<GraphicsDesignResult> SaveAsync()
    {
        if (_session.Busy) return new(false, "Graphics is busy. Try Save again in a moment.", null, null);
        if (_session.Draft is not { Declarative: true } draft || _session.TemplateDocument is not { } document)
            return new(false, "Open a design (or a look to copy) before saving.", null, null);
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
        var saved = await _designs.SaveAsync(design.DesignId, document);
        if (!saved.Ok || saved.Design is null)
            return saved with { Design = design, Message = prefix.Length > 0 ? prefix + "your edits were not saved: " + saved.Message : saved.Message };
        var reopened = await _designs.OpenAsync(saved.Design.DesignId);
        var message = prefix + saved.Message;
        if (reopened is { Ok: true, Template: { } current } && Show(current)) return saved with { Message = message, Template = current };
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
        return saved with { Message = saved.Message + $" \"{row.Name}\" is in the playlist{(row.Hotkey.Length > 0 ? " on " + row.Hotkey : "")}. Show it from Operate when you want it on air." };
    }

    /// <summary>The design's playlist row: its existing row re-pinned to <paramref name="template"/> (name,
    /// hotkey, timing, motion, names list and clock kept), else a new row on the next free F-key. The kind
    /// follows the template family (full screen saves as a title); a new lower third follows Sources and
    /// plays out after 8 s.</summary>
    public static GraphicsPlaylistRow PlaylistRow(GraphicsDesign design, NativeGraphicsTemplate template, NativeGraphicsDraft draft, IReadOnlyList<GraphicsPlaylistRow> rows)
    {
        var existing = rows.FirstOrDefault(r => r.TemplateId == design.TemplateId);
        var kind = GraphicsBuilder.Kinds.FirstOrDefault(k => k.Family.Length > 0 && k.Family == template.Family) ?? GraphicsBuilder.Kind("title")!;
        var lowerThird = kind.Family == "lower-third";
        var hotkey = existing is not null ? existing.Hotkey : GraphicsBuilder.NextFreeHotkey(rows);
        return GraphicsBuilder.RowFrom(kind, template, draft, existing?.Name ?? design.Name, hotkey, existing?.PlayOutAfterMs ?? (lowerThird ? 8000 : 0),
            existing?.FollowSources ?? lowerThird, rows, existing, existing?.Motion, existing?.Variants, existing?.Clock);
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
