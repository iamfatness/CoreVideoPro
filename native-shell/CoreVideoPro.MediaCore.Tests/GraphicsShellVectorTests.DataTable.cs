using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 6: table and crawl fields, manifests and bindings (GraphicsDataTable,
/// GraphicsCrawlData, GraphicsDataBinding.Valid, GraphicsDesignSeeds.Manifest). Every row carries
/// its dataset as the document's GraphicsLocalData JSON, so the Mac rebuilds the exact input.
/// Manifests are compared as parsed JSON objects (the core hashes its own canonical form).</summary>
public sealed partial class GraphicsShellVectorTests
{
    internal static GraphicsLocalData ScoresKeyed() =>
        GraphicsLocalData.ReadAsync(DataPath("data/scores.csv"), "team").GetAwaiter().GetResult();
    internal static GraphicsLocalData CrawlData() =>
        GraphicsLocalData.ReadAsync(DataPath("data/crawl.csv"), null).GetAwaiter().GetResult();

    private static JsonNode? Node<T>(T value) => JsonSerializer.SerializeToNode(value, Web);

    private static JsonObject Outcome(Func<JsonNode?> body)
    {
        try { return new JsonObject { ["value"] = body() }; }
        catch (InvalidDataException ex) { return new JsonObject { ["error"] = ex.Message }; }
    }

    [Fact]
    public void DataTableVectors()
    {
        var rows = new JsonArray();
        var scores = ScoresKeyed();
        var crawl = CrawlData();
        var wide = GraphicsLocalData.Parse(string.Join(",", Enumerable.Range(1, 9).Select(i => $"c{i}")) + "\n" +
            string.Join(",", Enumerable.Range(1, 9).Select(i => $"v{i}")) + "\n" + string.Join(",", Enumerable.Range(1, 9).Select(i => $"w{i}")), "csv", null);
        var escapes = GraphicsLocalData.Parse("label,n\n\"Tab\there\",1\n\"Quote \"\" and \\\\ back\",2\n\"<&>' + \u00e9 \U0001F600 \u2028 \u0085 \u007f\",3\n\"Line\nbreak\",4", "csv", null);
        // One code point per row: which characters UnsafeRelaxedJsonEscaping escapes.
        var probes = new[] { "\u00a0", "\u200b", "\u200d", "\ufeff", "\ue000", "\ufffd", "\u65e5", "\u0301", "\u2029", "\u0000", "\u001f", "\u0080", "\u009f",
            "\u00ad", "\u0378", "\u061c", "\u2066", "\ufff0", "\uffff", "\U0001F600", "\u00ff", "\u2122", "`", "~" };
        var escapeProbe = GraphicsLocalData.Parse("ch\n" + string.Join("\n", probes.Take(12).Select(p => "\"x" + p + "x\"")), "csv", null);
        var escapeProbe2 = GraphicsLocalData.Parse("ch\n" + string.Join("\n", probes.Skip(12).Select(p => "\"x" + p + "x\"")), "csv", null);
        var big = GraphicsLocalData.Parse("a,b\n" + new string('x', 2100) + "," + new string('y', 2100), "csv", null);
        var crawlEscapes = GraphicsLocalData.Parse("item\n\"<&>' + \\ back\"\nPlain\n\"Quote \"\" \u00e9 \U0001F600\"", "csv", null);
        var crawlEmpty = GraphicsLocalData.Parse("item\n\"\"\n  \n\"   \"", "csv", null);
        var crawlLong = GraphicsLocalData.Parse("item\n" + string.Join("\n", Enumerable.Range(1, 30).Select(i => new string((char)('a' + i % 26), 150))), "csv", null);
        var datasets = new Dictionary<string, GraphicsLocalData> {
            ["scores"] = scores, ["crawl"] = crawl, ["wide"] = wide, ["escapes"] = escapes, ["big"] = big,
            ["crawl-escapes"] = crawlEscapes, ["escape-probe"] = escapeProbe, ["escape-probe-2"] = escapeProbe2, ["crawl-empty"] = crawlEmpty, ["crawl-long"] = crawlLong };
        static GraphicsDataBinding Table(string source, string[] columns, string? sort, int limit, long revision = 1, bool remote = false, string? key = "team") =>
            new(source, key, columns, sort, limit, revision) { Remote = remote };
        static GraphicsDataBinding Crawl(string column, int limit, string direction = "left", string mode = "loop", long revision = 1, bool live = false) =>
            new("headlines", null, [column], null, limit, revision) { Crawl = new GraphicsCrawlOptions(120, 48, direction, mode), LiveFollow = live };

        var cases = new (string Id, string Data, GraphicsDataBinding Binding)[]
        {
            ("table-one-column-file-order", "scores", Table("scores", ["team"], null, 1)),
            ("table-two-columns-limit-12", "scores", Table("scores", ["team", "points"], null, 12)),
            ("table-sorted-points-limit-5", "scores", Table("scores", ["team", "points"], "points", 5)),
            ("table-sorted-four-columns-limit-12", "scores", Table("scores", ["city", "team", "qualified", "points"], "points", 12)),
            ("table-remote-adds-revision", "scores", Table("scores", ["team", "points"], null, 3, revision: 7, remote: true)),
            ("table-remote-sorted", "scores", Table("scores", ["team", "points"], "points", 3, revision: 2, remote: true)),
            ("table-eight-columns", "wide", Table("wide", ["c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8"], null, 2, key: null)),
            ("table-escaped-cells", "escapes", Table("escapes", ["label", "n"], null, 4, key: null)),
            ("table-escape-probe-1", "escape-probe", Table("escape-probe", ["ch"], null, 12, key: null)),
            ("table-escape-probe-2", "escape-probe-2", Table("escape-probe-2", ["ch"], null, 12, key: null)),
            ("refused-sort-on-text-column", "scores", Table("scores", ["team"], "city", 3)),
            ("refused-nine-columns", "wide", Table("wide", ["c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"], null, 2, key: null)),
            ("refused-seven-columns-plus-sort", "wide", Table("wide", ["c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8"], "c1", 2, key: null)),
            ("refused-row-over-4096-bytes", "big", Table("big", ["a", "b"], null, 1, key: null)),
            ("refused-missing-column", "scores", Table("scores", ["rank"], null, 3)),
            ("crawl-left-loop-limit-64", "crawl", Crawl("item", 64)),
            ("crawl-right-single-limit-1", "crawl", Crawl("item", 1, "right", "single", revision: 3, live: true)),
            ("crawl-escaped-items", "crawl-escapes", Crawl("item", 64)),
            ("refused-crawl-empty-result", "crawl-empty", Crawl("item", 64)),
            ("refused-crawl-item-with-tab", "escapes", Crawl("label", 64)),
            ("refused-crawl-number-column", "scores", Crawl("points", 64)),
            ("refused-crawl-too-long", "crawl-long", Crawl("item", 30)),
        };
        foreach (var (id, data, binding) in cases)
        {
            var input = new JsonObject { ["data"] = Node(datasets[data]), ["binding"] = Node(binding) };
            rows.Add(Row("fields-" + id, "fields", input.DeepClone(), Outcome(() => Node(GraphicsDataTable.Fields(datasets[data], binding)))));
            var manifestInput = input.DeepClone().AsObject();
            manifestInput["templateId"] = "design-scores-a1b2c3"; manifestInput["name"] = "Scores";
            rows.Add(Row("manifest-" + id, "manifest", manifestInput, Outcome(() => GraphicsDataTable.Manifest("design-scores-a1b2c3", "Scores", binding, datasets[data]))));
        }
        // Binding round trip through a manifest's metadata (GraphicsDataTable.Binding).
        foreach (var (id, data, binding) in cases.Where(c => !c.Id.StartsWith("refused")))
        {
            var manifest = GraphicsDataTable.Manifest("design-scores-a1b2c3", "Scores", binding, datasets[data]);
            rows.Add(Row("binding-from-manifest-" + id, "bindingFromDefinition", new JsonObject { ["definition"] = manifest.DeepClone() },
                new JsonObject { ["binding"] = Node(GraphicsDataTable.Binding(JsonSerializer.SerializeToElement(manifest))) }));
        }
        rows.Add(Row("binding-from-definition-without-metadata", "bindingFromDefinition", new JsonObject { ["definition"] = new JsonObject { ["templateId"] = "x" } },
            new JsonObject { ["binding"] = null }));
        // Valid(): every clause false once (rule 3), plus the valid shapes.
        var valid = Table("scores", ["team", "points"], "points", 5);
        var crawlValid = Crawl("item", 10);
        var validity = new (string Id, GraphicsDataBinding Binding)[]
        {
            ("table-valid", valid), ("crawl-valid", crawlValid),
            ("bad-source-id", valid with { SourceId = "Bad Id" }), ("source-id-49", valid with { SourceId = new string('s', 49) }),
            ("no-columns", valid with { Columns = [] }), ("nine-columns", valid with { Columns = Enumerable.Range(1, 9).Select(i => $"c{i}").ToArray(), SortColumn = null }),
            ("empty-column", valid with { Columns = ["team", ""] }), ("column-129", valid with { Columns = [new string('c', 129)] }),
            ("duplicate-columns", valid with { Columns = ["team", "team"] }), ("key-empty", valid with { KeyColumn = "" }),
            ("key-129", valid with { KeyColumn = new string('k', 129) }), ("sort-empty", valid with { SortColumn = "" }),
            ("sort-129", valid with { SortColumn = new string('s', 129) }),
            ("eight-columns-plus-sort", valid with { Columns = Enumerable.Range(1, 8).Select(i => $"c{i}").ToArray() }),
            ("seven-columns-plus-sort", valid with { Columns = Enumerable.Range(1, 7).Select(i => $"c{i}").ToArray() }),
            ("limit-0", valid with { Limit = 0 }), ("limit-13", valid with { Limit = 13 }), ("limit-12", valid with { Limit = 12 }),
            ("table-live-follow-without-remote", valid with { LiveFollow = true }), ("table-live-follow-remote", valid with { LiveFollow = true, Remote = true }),
            ("remote-without-key", valid with { Remote = true, KeyColumn = null }), ("revision-0", valid with { Revision = 0 }),
            ("crawl-speed-0", crawlValid with { Crawl = new GraphicsCrawlOptions(0) }), ("crawl-speed-1201", crawlValid with { Crawl = new GraphicsCrawlOptions(1201) }),
            ("crawl-gap-negative", crawlValid with { Crawl = new GraphicsCrawlOptions(120, -1) }), ("crawl-gap-513", crawlValid with { Crawl = new GraphicsCrawlOptions(120, 513) }),
            ("crawl-direction-up", crawlValid with { Crawl = new GraphicsCrawlOptions(120, 48, "up") }), ("crawl-mode-bounce", crawlValid with { Crawl = new GraphicsCrawlOptions(120, 48, "left", "bounce") }),
            ("crawl-two-columns", crawlValid with { Columns = ["item", "other"] }), ("crawl-with-sort", crawlValid with { SortColumn = "item" }),
            ("crawl-limit-64", crawlValid with { Limit = 64 }), ("crawl-limit-65", crawlValid with { Limit = 65 }),
            ("crawl-live-follow-local", crawlValid with { LiveFollow = true }), ("crawl-fractional-speed", crawlValid with { Crawl = new GraphicsCrawlOptions(0.5) }),
        };
        foreach (var (id, binding) in validity)
            rows.Add(Row("binding-valid-" + id, "bindingValid", new JsonObject { ["binding"] = Node(binding) }, new JsonObject { ["valid"] = binding.Valid() }));
        // GraphicsCrawlData.Items: trims, filters empties, THEN takes the limit.
        foreach (var (id, data, column, limit) in new (string, string, string, int)[] { ("crawl-64", "crawl", "item", 64), ("crawl-8", "crawl", "item", 8),
                     ("empties-filtered-before-limit", "crawl-empty", "item", 3), ("number-column-refused", "scores", "points", 5), ("missing-column-refused", "scores", "nope", 5) })
            rows.Add(Row("crawl-items-" + id, "crawlItems", new JsonObject { ["data"] = Node(datasets[data]), ["column"] = column, ["limit"] = limit },
                Outcome(() => Node(GraphicsCrawlData.Items(datasets[data], column, limit)))));
        // The seed every data manifest starts from (GraphicsDesignSeeds.Manifest("title", …)).
        rows.Add(Row("seed-title", "seed", new JsonObject { ["kind"] = "title", ["templateId"] = "design-scores-a1b2c3", ["templateVersion"] = 1, ["name"] = "Scores" },
            new JsonObject { ["manifest"] = GraphicsDesignSeeds.Manifest("title", "design-scores-a1b2c3", 1, "Scores") }));
        Check("data-table", "Data tables and crawls: GraphicsDataTable.Fields / Manifest / Binding, GraphicsDataBinding.Valid (every clause), GraphicsCrawlData.Items, GraphicsDesignSeeds.Manifest(\"title\").", rows);
    }
}
