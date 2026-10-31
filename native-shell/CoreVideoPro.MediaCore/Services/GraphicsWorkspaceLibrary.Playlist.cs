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

    /// <summary>Persist the playlist rows. Pins the exact installed definition of each
    /// referenced template when the native catalog advertises it; curated templates
    /// ship with the core, so a missing catalog entry does not block the save.</summary>
    public async Task<bool> SavePlaylistAsync(IReadOnlyList<GraphicsPlaylistRow> rows)
    {
        await LoadAsync();
        if (rows is null || rows.Count > 64 || rows.Any(r => r is null || !r.IsValid) || rows.Select(r => r.Id).Distinct(StringComparer.Ordinal).Count() != rows.Count)
            return Fail("The playlist has an invalid or duplicate row. Previous playlist retained.");
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
        return await Save(Document with { Playlist = rows.ToArray(), Templates = templates }, "Playlist saved. Program and Preview are unchanged.");
    }

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
