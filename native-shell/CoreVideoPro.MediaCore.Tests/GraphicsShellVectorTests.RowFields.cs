using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 1: which of a row's template variables the Operate inspector offers
/// (GraphicsRowFields.TemplateFieldsOf / Typeable, GraphicsMappedValues.Eligible) and the row's
/// Title line key (GraphicsPlayoutControl.TitleFieldKey).</summary>
public sealed partial class GraphicsShellVectorTests
{
    /// <summary>Restated from W/Controls/GraphicsPlayoutControl.cs:165-166 (a private WinUI helper the
    /// exporter cannot call): a Guest row's Title line is <c>title</c>, any other row's is the first of
    /// title / subtitle / label its fields carry.</summary>
    private static string? TitleFieldKey(GraphicsPlaylistRow row) => row.Kind == GraphicsPlaylistRow.Guest ? "title" :
        new[] { "title", "subtitle", "label" }.FirstOrDefault(row.Fields.ContainsKey);

    /// <summary>A data table design pinned by the real GraphicsDataTable.Manifest (3 visible rows).
    /// Fixture invariant: its fields are table-row-1..table-row-3, all strings, so a broken
    /// IsDataField filter would offer them as boxes.</summary>
    internal static (GraphicsPlaylistRow Row, GraphicsSavedTemplate Pin) DataTableRowAndPin()
    {
        var data = GraphicsLocalData.Parse("team,points\nAda,3\nBo,5\nCy,4\n", "csv", "team");
        var binding = new GraphicsDataBinding("scores", "team", ["team", "points"], null, 3, 1);
        var manifest = GraphicsDataTable.Manifest("design-scores-a1b2c3", "Scores", binding, data);
        var definition = JsonSerializer.SerializeToElement(manifest);
        var pin = new GraphicsSavedTemplate("design-scores-a1b2c3", 1, new string('c', 64), definition);
        var row = new GraphicsPlaylistRow
        {
            Id = "scores", Kind = GraphicsPlaylistRow.Title, Name = "Scores", TemplateId = pin.TemplateId, TemplateVersion = 1,
            Fields = GraphicsDataTable.Fields(data, binding), DataBinding = binding,
        };
        return (row, pin);
    }

    /// <summary>A Guest design pinned with extra variables: a labelled string (typed), a choice list
    /// (not typed), a number (mappable, not typed), name/title/time (handled).</summary>
    internal static (GraphicsPlaylistRow Row, GraphicsSavedTemplate Pin) GuestDesignRowAndPin()
    {
        var manifest = GraphicsDesignSeeds.Manifest("title", "design-guest-d4e5f6", 1, "Guest design");
        manifest["fields"] = new JsonArray(
            new JsonObject { ["id"] = "name", ["type"] = "string", ["maxLength"] = 128 },
            new JsonObject { ["id"] = "title", ["type"] = "string", ["maxLength"] = 256 },
            new JsonObject { ["id"] = "location", ["type"] = "string", ["maxLength"] = 64, ["metadata"] = new JsonObject { ["label"] = "City and venue" } },
            new JsonObject { ["id"] = "pronoun_line", ["type"] = "string" },
            new JsonObject { ["id"] = "mood", ["type"] = "string", ["choices"] = new JsonArray("calm", "loud") },
            new JsonObject { ["id"] = "seat-count", ["type"] = "number" },
            new JsonObject { ["id"] = "accent", ["type"] = "color" },
            new JsonObject { ["id"] = "time", ["type"] = "string" });
        var pin = new GraphicsSavedTemplate("design-guest-d4e5f6", 1, new string('d', 64), JsonSerializer.SerializeToElement(manifest));
        var row = new GraphicsPlaylistRow
        {
            Id = "guest-design", Kind = GraphicsPlaylistRow.Guest, Name = "Guest design", FollowSources = true,
            TemplateId = pin.TemplateId, TemplateVersion = 1,
        };
        return (row, pin);
    }

    [Fact]
    public void RowFieldsVectors()
    {
        var catalog = RealCatalog();
        var rows = new JsonArray();
        void Add(string id, GraphicsPlaylistRow row, NativeGraphicsState? state, string? nodeFile, IReadOnlyList<GraphicsSavedTemplate> pins)
        {
            var titleKey = TitleFieldKey(row);
            var fields = GraphicsRowFields.TemplateFieldsOf(row, state, pins);
            var typeable = GraphicsRowFields.Typeable(row, state, pins, row.Kind == GraphicsPlaylistRow.Guest ? null : titleKey);
            rows.Add(Row(id, "rowFields",
                new JsonObject
                {
                    ["row"] = JsonSerializer.SerializeToNode(row, Web), ["nodeFile"] = nodeFile,
                    ["pins"] = JsonSerializer.SerializeToNode(pins, Web),
                },
                new JsonObject
                {
                    ["titleFieldKey"] = titleKey,
                    ["templateFields"] = new JsonArray(fields.Select(f => (JsonNode)f.Id).ToArray()),
                    ["typeable"] = new JsonArray(typeable.Select(f => (JsonNode)new JsonObject { ["id"] = f.Id, ["label"] = f.Label, ["maxLength"] = f.MaxLength }).ToArray()),
                    ["eligible"] = new JsonArray(fields.Where(GraphicsMappedValues.Eligible).Select(f => (JsonNode)f.Id).ToArray()),
                }));
        }
        var show = JsonSerializer.Deserialize<GraphicsShowDocument>(WindowsShowDocument(), Store)!;
        foreach (var row in show.Playlist) Add("windows-show-" + row.Id, row, catalog, "nodes/program-and-cue.json", show.Templates);
        var (names, clock) = NamesAndClockRows();
        foreach (var row in new[] { names, clock }) Add("windows-names-clock-" + row.Id, row, catalog, "nodes/program-and-cue.json", []);
        var (table, tablePin) = DataTableRowAndPin();
        Add("data-table-pin", table, null, null, [tablePin]);
        Add("data-table-without-binding-offers-row-fields", table with { DataBinding = null }, null, null, [tablePin]);
        var (guest, guestPin) = GuestDesignRowAndPin();
        Add("guest-design-pin", guest, null, null, [guestPin]);
        Add("guest-design-pin-version-mismatch", guest with { TemplateVersion = 2 }, null, null, [guestPin]);
        Add("image-pin-wins-over-template", new GraphicsPlaylistRow { Id = "card", Kind = GraphicsPlaylistRow.Sponsor, Name = "Card",
            Image = new NativeGraphicsImageReference(guestPin.TemplateId, 1, "asset-1") }, null, null, [guestPin]);
        Add("title-row-label-key", clock with { Kind = GraphicsPlaylistRow.Title, Fields = new Dictionary<string, JsonElement>(StringComparer.Ordinal) {
            ["label"] = JsonSerializer.SerializeToElement("LIVE"), ["text-size"] = JsonSerializer.SerializeToElement(40) } }, catalog, "nodes/program-and-cue.json", []);
        Add("compatibility-template-has-none", show.Playlist[0] with { FollowSources = false }, catalog, "nodes/program-and-cue.json", show.Templates);
        Check("rowfields", "Operate inspector fields: GraphicsRowFields.TemplateFieldsOf / Typeable (title key per GraphicsPlayoutControl.TitleFieldKey; a Guest row passes none), GraphicsMappedValues.Eligible.", rows);
    }
}
