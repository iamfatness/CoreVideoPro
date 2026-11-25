using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 13: Shot Layouts. documents/windows-layouts.json is written by the real
/// serializer (the four presets over the interview rows plus the windows-data rows, a layout with
/// rowSettings (the data table pinned to revision 2, the title's heading overridden), a layout whose override does not fit its template, a layout
/// naming a row the playlist lacks). Fixture invariants: every layout is ShotLayout.IsValid (so the
/// store loads it); "open" names scene "intro", which the Mac scene list lacks.</summary>
public sealed partial class GraphicsShellVectorTests
{
    internal static GraphicsShowDocument WindowsLayoutsShow()
    {
        var data = WindowsDataShow();
        var interview = InterviewRows();
        var playlist = interview.Concat(data.Playlist).ToArray();
        static ShotLayoutRowSettings Settings(long? revision, params (string Key, object Value)[] overrides) => new()
        {
            DataPin = revision is { } r ? new ShotLayoutDataPin("scores", r) : null,
            Overrides = overrides.ToDictionary(o => o.Key, o => JsonSerializer.SerializeToElement(o.Value), StringComparer.Ordinal),
        };
        var layouts = GraphicsWorkspaceLibrary.DefaultShotLayouts().Concat(new ShotLayout[]
        {
            new() { Id = "final-scores", Name = "Final scores", SceneId = "", RowIds = ["leaderboard", "title"], Hotkey = "Shift+F4",
                RowSettings = new Dictionary<string, ShotLayoutRowSettings>(StringComparer.Ordinal) { ["leaderboard"] = Settings(2), ["title"] = Settings(null, ("heading", "Final")) } },
            new() { Id = "bad-override", Name = "Bad override", SceneId = "", RowIds = ["title"], Hotkey = "",
                RowSettings = new Dictionary<string, ShotLayoutRowSettings>(StringComparer.Ordinal) { ["title"] = Settings(null, ("text-size", "huge")) } },
            new() { Id = "ghost-row", Name = "Ghost row", SceneId = "", RowIds = ["nowhere"], Hotkey = "Shift+F5" },
        }).ToArray();
        return data with { Playlist = playlist, ShotLayouts = layouts };
    }

    /// <summary>Restated from W/Services/GraphicsPlayoutActions.cs LayoutSet (a WinUI action the exporter
    /// cannot call): validity, the scene check, the hotkey move, then the real SaveShotLayoutsAsync.</summary>
    private static JsonObject SetLayout(GraphicsWorkspaceLibrary library, string json, IReadOnlyCollection<string> scenes)
    {
        ShotLayout? layout;
        try { layout = JsonSerializer.Deserialize<ShotLayout>(json, Web); }
        catch (JsonException ex) { return new JsonObject { ["ok"] = false, ["error"] = ex.Message }; }
        if (layout is null || !layout.IsValid) return new JsonObject { ["ok"] = false, ["error"] = "That shot layout is not valid." };
        if (layout.SceneId.Length > 0 && !scenes.Contains(layout.SceneId)) return new JsonObject { ["ok"] = false, ["error"] = $"Scene '{layout.SceneId}' is not in this show." };
        var layouts = library.Document.ShotLayouts.Select(l => l.Id != layout.Id && layout.Hotkey.Length > 0 && l.Hotkey == layout.Hotkey ? l with { Hotkey = "" } : l).ToList();
        var index = layouts.FindIndex(l => l.Id == layout.Id);
        if (index < 0) layouts.Add(layout); else layouts[index] = layout;
        var ok = library.SaveShotLayoutsAsync(layouts).GetAwaiter().GetResult();
        return new JsonObject
        {
            ["ok"] = ok, ["error"] = ok ? null : library.Status,
            ["layouts"] = new JsonArray(library.Document.ShotLayouts.Select(l => (JsonNode)new JsonObject { ["id"] = l.Id, ["hotkey"] = l.Hotkey }).ToArray()),
        };
    }

    [Fact]
    public void LayoutVectors()
    {
        var show = WindowsLayoutsShow();
        var text = Serialize(show);
        var fixture = DocumentPath("windows-layouts.json");
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS") == "1") File.WriteAllText(fixture, text, new UTF8Encoding(false));
        else Assert.True(File.Exists(fixture) && File.ReadAllText(fixture).Replace("\r\n", "\n") == text, $"{fixture} differs; regenerate.");

        var rows = new JsonArray();
        // IsValid: the fixture's layouts, and each clause false once (rule 3).
        var baseLayout = new JsonObject { ["id"] = "base", ["name"] = "Base", ["sceneId"] = "", ["rowIds"] = new JsonArray("title", "leaderboard"),
            ["rowSettings"] = new JsonObject { ["leaderboard"] = new JsonObject { ["dataPin"] = new JsonObject { ["sourceId"] = "scores", ["revision"] = 2 },
                ["overrides"] = new JsonObject { ["heading"] = "Final", ["text-size"] = 40, ["show-logo"] = true } } },
            ["hotkey"] = "Shift+F1", ["playOutOthers"] = false };
        JsonObject With(Action<JsonObject> change) { var copy = baseLayout.DeepClone().AsObject(); change(copy); return copy; }
        JsonObject Settings(JsonObject layout) => layout["rowSettings"]!["leaderboard"]!.AsObject();
        var cases = new List<(string Id, JsonObject Layout)> { ("base", baseLayout) };
        foreach (var layout in show.ShotLayouts) cases.Add(("fixture-" + layout.Id, JsonSerializer.SerializeToNode(layout, Web)!.AsObject()));
        cases.AddRange(new (string, JsonObject)[] {
            ("bad-id", With(l => l["id"] = "Bad Id")), ("id-49", With(l => l["id"] = new string('i', 49))),
            ("name-empty", With(l => l["name"] = "")), ("name-129", With(l => l["name"] = new string('n', 129))),
            ("scene-65", With(l => l["sceneId"] = new string('s', 65))), ("hotkey-shift-f12", With(l => l["hotkey"] = "Shift+F12")),
            ("hotkey-f1", With(l => l["hotkey"] = "F1")), ("hotkey-shift-f11", With(l => l["hotkey"] = "Shift+F11")),
            ("rows-17", With(l => { l["rowIds"] = new JsonArray(Enumerable.Range(1, 17).Select(i => (JsonNode)$"row-{i}").ToArray()); l["rowSettings"] = new JsonObject(); })),
            ("rows-16", With(l => { l["rowIds"] = new JsonArray(Enumerable.Range(1, 16).Select(i => (JsonNode)$"row-{i}").ToArray()); l["rowSettings"] = new JsonObject(); })),
            ("rows-duplicate", With(l => { l["rowIds"] = new JsonArray("title", "title"); l["rowSettings"] = new JsonObject(); })),
            ("rows-bad-id", With(l => { l["rowIds"] = new JsonArray("Title"); l["rowSettings"] = new JsonObject(); })),
            ("settings-key-not-in-rows", With(l => l["rowIds"] = new JsonArray("title"))),
            ("pin-bad-source", With(l => Settings(l)["dataPin"]!["sourceId"] = "Bad Source")),
            ("pin-revision-0", With(l => Settings(l)["dataPin"]!["revision"] = 0)),
            ("no-pin-no-overrides", With(l => { Settings(l)["dataPin"] = null; Settings(l)["overrides"] = new JsonObject(); })),
            ("overrides-only", With(l => Settings(l)["dataPin"] = null)),
            ("overrides-33", With(l => Settings(l)["overrides"] = new JsonObject(Enumerable.Range(1, 33).Select(i => new KeyValuePair<string, JsonNode?>($"k{i}", i))))),
            ("override-bad-key", With(l => Settings(l)["overrides"]!["Bad Key"] = "x")),
            ("override-object", With(l => Settings(l)["overrides"]!["heading"] = new JsonObject { ["a"] = 1 })),
            ("override-array", With(l => Settings(l)["overrides"]!["heading"] = new JsonArray(1))),
            ("override-null", With(l => Settings(l)["overrides"]!["heading"] = null)),
            ("override-4097-bytes", With(l => Settings(l)["overrides"]!["heading"] = new string('é', 2049))),
            ("override-4096-bytes", With(l => Settings(l)["overrides"]!["heading"] = new string('é', 2048))),
        });
        foreach (var (id, layout) in cases)
        {
            bool valid;
            try { valid = JsonSerializer.Deserialize<ShotLayout>(layout.ToJsonString(), Web)?.IsValid == true; }
            catch (JsonException) { valid = false; }
            rows.Add(Row("valid-" + id, "layoutValid", new JsonObject { ["layout"] = layout.DeepClone() }, new JsonObject { ["valid"] = valid }));
        }
        // Slug (GraphicsPlayoutControl.Slug, restated: a private WinUI helper).
        static string Slug(string name, IEnumerable<string> taken)
        {
            var chars = name.ToLowerInvariant().Select(c => c is >= 'a' and <= 'z' or >= '0' and <= '9' ? c : '-').ToArray();
            var slug = string.Join('-', new string(chars).Split('-', StringSplitOptions.RemoveEmptyEntries));
            if (slug.Length == 0 || slug[0] is < 'a' or > 'z') slug = "layout" + (slug.Length == 0 ? "" : "-" + slug);
            if (slug.Length > 40) slug = slug[..40].TrimEnd('-');
            var set = taken.ToHashSet(StringComparer.Ordinal); var candidate = slug; var n = 2;
            while (set.Contains(candidate)) candidate = slug + "-" + n++;
            return candidate;
        }
        foreach (var (id, name, taken) in new (string, string, string[])[] { ("two-up", "Two-up!", []), ("digit-first", "9 Panel", []), ("empty", "", []),
                     ("long", new string('a', 30) + " " + new string('b', 30), []), ("taken", "Open", ["open", "open-2"]), ("non-ascii", "Zoë Ç", []) })
            rows.Add(Row("slug-" + id, "slug", new JsonObject { ["name"] = name, ["taken"] = JsonSerializer.SerializeToNode(taken) }, new JsonObject { ["slug"] = Slug(name, taken) }));
        // setLayout outcomes against windows-layouts.json, Mac scene ids (owner decision 2: presets keep Windows ids).
        var macScenes = new[] { "fullscreen", "two-up", "pip" };
        var directory = Path.Combine(Path.GetTempPath(), "cvp-layouts-" + Guid.NewGuid().ToString("N"));
        try
        {
            foreach (var (id, layout, dropGhost) in new (string, JsonObject, bool)[] {
                         ("new-layout-takes-a-hotkey", new JsonObject { ["id"] = "pip-look", ["name"] = "PiP look", ["sceneId"] = "pip", ["rowIds"] = new JsonArray("guest"), ["hotkey"] = "Shift+F2" }, true),
                         ("replace-existing", new JsonObject { ["id"] = "clean", ["name"] = "Clean", ["sceneId"] = "", ["rowIds"] = new JsonArray(), ["hotkey"] = "Shift+F10", ["playOutOthers"] = true }, true),
                         ("scene-not-on-mac", new JsonObject { ["id"] = "open", ["name"] = "Open", ["sceneId"] = "intro", ["rowIds"] = new JsonArray("title"), ["hotkey"] = "Shift+F1" }, true),
                         ("invalid", new JsonObject { ["id"] = "x", ["name"] = "", ["sceneId"] = "", ["rowIds"] = new JsonArray() }, true),
                         ("missing-row-in-another-layout-blocks-save", new JsonObject { ["id"] = "solo", ["name"] = "Solo", ["sceneId"] = "", ["rowIds"] = new JsonArray("logo") }, false),
                         ("row-not-in-playlist", new JsonObject { ["id"] = "solo", ["name"] = "Solo", ["sceneId"] = "", ["rowIds"] = new JsonArray("missing-row") }, true) })
            {
                var path = Path.Combine(directory, id, "show.json");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                // dropGhost: the fixture minus its ghost-row layout, so a save can succeed.
                File.WriteAllText(path, dropGhost ? Serialize(show with { ShotLayouts = show.ShotLayouts.Where(l => l.Id != "ghost-row").ToArray() }) : text, new UTF8Encoding(false));
                var session = new GraphicsWorkspaceSession((_, _, _) => Task.FromResult(new NativeGraphicsOperationResult()), () => null);
                var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(path), session);
                library.LoadAsync().GetAwaiter().GetResult();
                rows.Add(Row("set-" + id, "setLayout", new JsonObject { ["document"] = "documents/windows-layouts.json", ["layout"] = layout.DeepClone(), ["dropLayouts"] = dropGhost ? new JsonArray("ghost-row") : new JsonArray(),
                    ["scenes"] = JsonSerializer.SerializeToNode(macScenes) }, SetLayout(library, layout.ToJsonString(), macScenes)));
            }
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, true); }
        Check("layouts", "Shot Layouts: ShotLayout.IsValid / ShotLayoutRowSettings.IsValid (each clause), the layout slug, and set-layout outcomes (GraphicsPlayoutActions LayoutSet + SaveShotLayoutsAsync) over documents/windows-layouts.json with the Mac scene ids.", rows);
    }
}
