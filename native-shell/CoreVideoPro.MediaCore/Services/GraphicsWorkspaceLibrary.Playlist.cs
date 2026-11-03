using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsWorkspaceLibrary
{
    /// <summary>The five default rows for a show starter, built from the same curated
    /// templates and sample copy that <see cref="GraphicsShowStarters.Prepare"/> uses.
    /// Hotkeys F1..F5 in row order. Pure; nothing is created, cued or shown.</summary>
    public static IReadOnlyList<GraphicsPlaylistRow> DefaultPlaylistFor(GraphicsShowStarter starter, NativeGraphicsState state)
    {
        var graphics = GraphicsShowStarters.Prepare(starter, state);
        GraphicsPlaylistRow Row(string id, string kind, string name, string hotkey, int playOutAfterMs, GraphicsStarterGraphic? graphic, bool followSources = false, bool showClock = false) => new() {
            Id = id, Kind = kind, Name = name, Hotkey = hotkey, PlayOutAfterMs = playOutAfterMs,
            TemplateId = graphic?.Draft.TemplateId ?? "", TemplateVersion = graphic?.Draft.TemplateVersion ?? 0,
            Fields = graphic is null ? new Dictionary<string, JsonElement>(StringComparer.Ordinal)
                : graphic.Draft.Fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal),
            Style = graphic?.Draft.Style ?? LowerThirdAppearance.FromPreset(starter.LowerThird) with { AccentColor = starter.Accent },
            FollowSources = followSources, ShowClock = showClock
        };
        var liveBug = graphics[3];
        var liveFields = liveBug.Draft.Fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone(), StringComparer.Ordinal);
        if (liveFields.ContainsKey("label")) liveFields["label"] = JsonSerializer.SerializeToElement("LIVE");
        return [
            Row("guest", GraphicsPlaylistRow.Guest, "Guest name", "F1", 8000, graphics[0], followSources: true),
            Row("title", GraphicsPlaylistRow.Title, "Show title", "F2", 0, graphics[1]),
            Row("logo", GraphicsPlaylistRow.Logo, "Logo", "F3", 0, graphics[2]),
            Row("live-bug", GraphicsPlaylistRow.LiveBug, "LIVE bug", "F4", 0, liveBug, showClock: true) with { Fields = liveFields },
            // Full-canvas image placeholder: AssignPlaylistImageAsync pins the verified package.
            Row("sponsor", GraphicsPlaylistRow.Sponsor, "Sponsor card", "F5", 15000, null)
        ];
    }

    /// <summary>The four preset shot layouts that ship with the default playlist (#1076 slice 2).
    /// Scene ids are the production catalog's (intro, interview, panel); a layout whose scene is
    /// missing from the show is refused at cue time, never skipped. Clean changes no scene and
    /// plays everything out; Shift+F12 is already Play Out all, so Clean sits on Shift+F11.</summary>
    public static IReadOnlyList<ShotLayout> DefaultShotLayouts() => [
        new() { Id = "open", Name = "Open", SceneId = "intro", RowIds = ["title", "logo"], Hotkey = "Shift+F1" },
        new() { Id = "two-up", Name = "Two-up", SceneId = "interview", RowIds = ["guest"], Hotkey = "Shift+F2" },
        new() { Id = "panel", Name = "Panel", SceneId = "panel", RowIds = ["guest", "logo"], Hotkey = "Shift+F3" },
        new() { Id = "clean", Name = "Clean", SceneId = "", RowIds = [], Hotkey = "Shift+F11", PlayOutOthers = true },
    ];

    /// <summary>Persist the playlist rows. Pins the exact installed definition of each
    /// referenced template when the native catalog advertises it; curated templates
    /// ship with the core, so a missing catalog entry does not block the save. When
    /// <paramref name="layouts"/> is given it is saved in the same write (the default
    /// playlist installs its preset layouts this way).</summary>
    public async Task<bool> SavePlaylistAsync(IReadOnlyList<GraphicsPlaylistRow> rows, IReadOnlyList<ShotLayout>? layouts = null)
    {
        await LoadAsync();
        if (rows is null || rows.Count > 64 || rows.Any(r => r is null || !r.IsValid) || rows.Select(r => r.Id).Distinct(StringComparer.Ordinal).Count() != rows.Count)
            return Fail("The playlist has an invalid or duplicate row. Previous playlist retained.");
        if (layouts is not null && !ValidLayouts(layouts)) return Fail("A shot layout is invalid or duplicated. Previous playlist retained.");
        var templates = Document.Templates.ToList();
        foreach (var key in rows.Where(r => r.TemplateId.Length > 0).Select(r => (Id: r.TemplateId, Version: r.TemplateVersion))
                     .Concat(rows.Where(r => r.Image is not null).Select(r => (Id: r.Image!.TemplateId, Version: r.Image.TemplateVersion))).Distinct()) {
            var installed = _session.State?.Templates.FirstOrDefault(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
            if (installed?.Definition is not { } definition) continue;
            var index = templates.FindIndex(t => t.TemplateId == key.Id && t.TemplateVersion == key.Version);
            if (index >= 0 && templates[index].ManifestHash != installed.ManifestHash) return Fail("A playlist template pin conflicts with the installed version. Previous playlist retained.");
            var pin = new GraphicsSavedTemplate(key.Id, key.Version, installed.ManifestHash, definition.Clone(), installed.PackageRoot);
            if (index < 0) templates.Add(pin); else templates[index] = pin;
        }
        return await Save(Document with { Playlist = rows.ToArray(), Templates = templates, ShotLayouts = layouts?.ToArray() ?? Document.ShotLayouts }, "Playlist saved. Program and Preview are unchanged.");
    }

    /// <summary>Persist the shot layouts. Row ids must exist in the saved playlist; one hotkey
    /// belongs to one layout (the caller moves a key before saving).</summary>
    public async Task<bool> SaveShotLayoutsAsync(IReadOnlyList<ShotLayout> layouts)
    {
        await LoadAsync();
        if (!ValidLayouts(layouts)) return Fail("A shot layout is invalid or duplicated. Previous layouts retained.");
        var missing = layouts.SelectMany(l => l.RowIds).Distinct(StringComparer.Ordinal).Where(id => Document.Playlist.All(r => r.Id != id)).ToArray();
        if (missing.Length > 0) return Fail($"Shot layouts reference playlist rows that do not exist: {string.Join(", ", missing)}. Previous layouts retained.");
        var keys = layouts.Where(l => l.Hotkey.Length > 0).Select(l => l.Hotkey).ToArray();
        if (keys.Distinct(StringComparer.Ordinal).Count() != keys.Length) return Fail("Two shot layouts share a hotkey. Previous layouts retained.");
        return await Save(Document with { ShotLayouts = layouts.ToArray() }, "Shot layouts saved. Program and Preview are unchanged.");
    }

    private static bool ValidLayouts(IReadOnlyList<ShotLayout> layouts) =>
        layouts is not null && layouts.Count <= 32 && layouts.All(l => l is { IsValid: true }) && layouts.Select(l => l.Id).Distinct(StringComparer.Ordinal).Count() == layouts.Count;

    /// <summary>Import a local image as a verified full-canvas package and pin it to the row.</summary>
    public async Task<bool> AssignPlaylistImageAsync(string rowId, string path, CancellationToken cancellation = default)
    {
        await LoadAsync();
        var rows = Document.Playlist.ToList(); var index = rows.FindIndex(r => r.Id == rowId);
        if (index < 0) return Fail("That playlist row no longer exists.");
        var reference = await _session.ImportImageAsync(path, cancellation, fullCanvas: true);
        if (reference is null) return Fail(_session.ImportStatus);
        rows[index] = rows[index] with { ImagePath = path, Image = reference, TemplateId = reference.TemplateId, TemplateVersion = reference.TemplateVersion,
            Fields = new Dictionary<string, JsonElement>(StringComparer.Ordinal) };
        return await SavePlaylistAsync(rows);
    }
}
