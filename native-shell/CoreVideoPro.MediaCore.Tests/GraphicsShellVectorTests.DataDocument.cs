using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 7: a show with datasets and data designs, written by the real Windows
/// serializer (documents/windows-data.json), and the store's refusals for invalid data sections.
/// Fixture invariants: every row binding's sourceId names a dataSources entry; each row's template
/// id is a design's template id; the table row's binding revision (1) is below its source's (2),
/// so publishing has work to do; the crawl row follows live.</summary>
public sealed partial class GraphicsShellVectorTests
{
    internal static readonly DateTimeOffset FixtureTime = new(2026, 11, 22, 12, 0, 0, TimeSpan.Zero);

    internal static GraphicsShowDocument WindowsDataShow()
    {
        var scores = ScoresKeyed(); var crawl = CrawlData();
        var table = new GraphicsDataBinding("scores", "team", ["team", "points"], "points", 5, 1);
        var ticker = new GraphicsDataBinding("headlines", null, ["item"], null, 64, 1) { Crawl = new GraphicsCrawlOptions(), LiveFollow = true };
        var tableId = GraphicsDesignIds.NewTemplateId("Leaderboard", [], () => "a1b2c3");
        var crawlId = GraphicsDesignIds.NewTemplateId("Headlines", [tableId], () => "d4e5f6");
        var tableManifest = GraphicsDataTable.Manifest(tableId, "Leaderboard", table, scores);
        var crawlManifest = GraphicsDataTable.Manifest(crawlId, "Headlines", ticker, crawl);
        return new GraphicsShowDocument
        {
            ShowId = "default", Authority = "legacy",
            Playlist = [
                new GraphicsPlaylistRow { Id = "leaderboard", Kind = GraphicsPlaylistRow.Title, Name = "Leaderboard", Hotkey = "F1",
                    TemplateId = tableId, TemplateVersion = 1, Fields = GraphicsDataTable.Fields(scores, table), DataBinding = table },
                new GraphicsPlaylistRow { Id = "headlines", Kind = GraphicsPlaylistRow.Title, Name = "Headlines", Hotkey = "F2",
                    TemplateId = crawlId, TemplateVersion = 1, Fields = GraphicsDataTable.Fields(crawl, ticker), DataBinding = ticker },
            ],
            Designs = [
                new GraphicsDesign(tableId, "Leaderboard", "title", tableId, 1, JsonSerializer.SerializeToElement(tableManifest), [], FixtureTime),
                new GraphicsDesign(crawlId, "Headlines", "title", crawlId, 1, JsonSerializer.SerializeToElement(crawlManifest), [], FixtureTime),
            ],
            DataSources = [
                // Real Windows writes 7 fractional digits; one source also carries a non-UTC offset (review I3).
                new GraphicsDataSource("scores", "scores", 2, FixtureTime.AddTicks(1234567), scores),
                new GraphicsDataSource("headlines", "crawl", 1, new DateTimeOffset(2026, 11, 22, 7, 0, 0, TimeSpan.FromHours(-5)).AddTicks(10), crawl),
            ],
            Metadata = new() { ["futureSection"] = JsonSerializer.SerializeToElement(new { keep = true }) },
        };
    }

    internal static string Serialize(GraphicsShowDocument document) =>
        Encoding.UTF8.GetString(JsonSerializer.SerializeToUtf8Bytes(document, Store)).Replace("\r\n", "\n") + "\n";

    [Fact]
    public void DataDocumentVectors()
    {
        var show = WindowsDataShow();
        var text = Serialize(show);
        var fixture = DocumentPath("windows-data.json");
        if (Environment.GetEnvironmentVariable("COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS") == "1") File.WriteAllText(fixture, text, new UTF8Encoding(false));
        else Assert.True(File.Exists(fixture) && File.ReadAllText(fixture).Replace("\r\n", "\n") == text, $"{fixture} differs; regenerate.");

        var rows = new JsonArray();
        var directory = Path.Combine(Path.GetTempPath(), "cvp-data-document-" + Guid.NewGuid().ToString("N"));
        try
        {
            var table = show.Playlist[0].DataBinding!;
            var cases = new (string Id, GraphicsShowDocument? Document)[]
            {
                ("windows-data", null),
                ("source-revision-0", show with { DataSources = [show.DataSources[0] with { Revision = 0 }, show.DataSources[1]] }),
                ("duplicate-source-ids", show with { DataSources = [show.DataSources[0], show.DataSources[0]] }),
                ("source-name-empty", show with { DataSources = [show.DataSources[0] with { Name = "" }, show.DataSources[1]] }),
                ("source-with-feed-sequence-but-no-feed", show with { DataSources = [show.DataSources[0] with { FeedSequence = 3 }, show.DataSources[1]] }),
                ("source-data-invalid", show with { DataSources = [show.DataSources[0] with { Data = show.DataSources[0].Data with { Rows = [] , Columns = [] } }, show.DataSources[1]] }),
                ("binding-nine-columns", show with { Playlist = [show.Playlist[0] with { DataBinding = table with { Columns = Enumerable.Range(1, 9).Select(i => $"c{i}").ToArray(), SortColumn = null } }, show.Playlist[1]] }),
                ("remote-binding-without-key", show with { Playlist = [show.Playlist[0] with { DataBinding = table with { Remote = true, KeyColumn = null } }, show.Playlist[1]] }),
                ("binding-on-a-guest-row", show with { Playlist = [show.Playlist[0] with { Kind = GraphicsPlaylistRow.Guest }, show.Playlist[1]] }),
                ("design-bad-template-id", show with { Designs = [show.Designs[0] with { TemplateId = "leaderboard" }, show.Designs[1]] }),
                ("design-name-81", show with { Designs = [show.Designs[0] with { Name = new string('n', 81) }, show.Designs[1]] }),
                ("design-duplicate-template-ids", show with { Designs = [show.Designs[0], show.Designs[0] with { DesignId = "other" }] }),
                ("design-version-0", show with { Designs = [show.Designs[0] with { TemplateVersion = 0 }, show.Designs[1]] }),
            };
            foreach (var (id, document) in cases)
            {
                var path = Path.Combine(directory, id, "default.json");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllText(path, document is null ? text : Serialize(document), new UTF8Encoding(false));
                var load = new GraphicsDocumentStore(path).LoadAsync().GetAwaiter().GetResult();
                var input = document is null ? new JsonObject { ["primaryFile"] = "documents/windows-data.json" } : new JsonObject { ["primary"] = Serialize(document) };
                rows.Add(Row("load-" + id, "load", input, new JsonObject
                {
                    ["readOnly"] = load.ReadOnly, ["recoveredBackup"] = load.RecoveredBackup, ["status"] = load.Status,
                    ["showId"] = load.Document.ShowId, ["authority"] = load.Document.Authority,
                    ["playlistIds"] = new JsonArray(load.Document.Playlist.Select(r => (JsonNode)r.Id).ToArray()),
                    ["dataSourceIds"] = new JsonArray(load.Document.DataSources.Select(s => (JsonNode)s.Id).ToArray()),
                    ["designIds"] = new JsonArray(load.Document.Designs.Select(d => (JsonNode)d.DesignId).ToArray()),
                }));
            }
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, true); }
        foreach (var (id, name, existing) in new (string, string, string[])[] { ("simple", "Leaderboard", []), ("non-ascii", "Élan Ç", []), ("empty", "  !! ", []),
                     ("long", new string('x', 60) + " tail", []), ("taken-once", "Leaderboard", ["design-leaderboard-a1b2c3"]) })
        {
            var sequence = new Queue<string>(["a1b2c3", "d4e5f6"]);
            rows.Add(Row("design-id-" + id, "newTemplateId", new JsonObject { ["name"] = name, ["existing"] = JsonSerializer.SerializeToNode(existing), ["hex6"] = new JsonArray("a1b2c3", "d4e5f6") },
                new JsonObject { ["templateId"] = GraphicsDesignIds.NewTemplateId(name, existing, () => sequence.Dequeue()) }));
        }
        Check("data-document", "Show documents with datasets and data designs: documents/windows-data.json (real serializer) and GraphicsDocumentStore refusals for invalid dataSources, bindings and designs; GraphicsDesignIds.NewTemplateId.", rows);
    }
}
