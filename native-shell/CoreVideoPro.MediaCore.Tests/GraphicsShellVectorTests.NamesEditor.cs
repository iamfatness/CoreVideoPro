using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 2: the names-list editor's parser and writer
/// (GraphicsBuilder.ParseNamesList / NamesListText / NewRowId). Text the writer returns is
/// normalized to \n (plan D12: Environment.NewLine is \r\n on Windows, and the area file's own
/// CRLF normalization never reaches an escaped \r\n inside a JSON string).</summary>
public sealed partial class GraphicsShellVectorTests
{
    private static GraphicsPlaylistVariant Variant(string id, string name, string title, params (string Key, string Value)[] cells) =>
        new() { Id = id, Name = name, Title = title, Cells = cells.ToDictionary(c => c.Key, c => c.Value, StringComparer.Ordinal) };

    [Fact]
    public void NamesEditorVectors()
    {
        var rows = new JsonArray();
        var existing = new[] { Variant("ada-lovelace", "Ada Lovelace", "Mathematician"), Variant("bo", "Bo", "Guitar"), Variant("cy-2", "Cy", "") };
        var many = string.Join("\n", Enumerable.Range(1, 70).Select(n => $"Guest {n}, Role {n}"));
        var cases = new (string Id, string Text, GraphicsPlaylistVariant[]? Existing, string[]? VariableIds)[]
        {
            ("plain-name-title", "Ada Lovelace, Mathematician\nBo Diddley,\nCy Twombly", null, null),
            ("tab-and-semicolon", "Ada\tMathematician\nBo;Guitar\nCy , Painter ", null, null),
            ("quoted-first-cell-with-comma", "\"Lovelace, Ada\", Analyst\n\"Diddley, Bo\"\n\"Say \"\"Hi\"\"\" , \"Greeter\"", null, null),
            ("two-column-header-skipped", "Name, Title\nAda, Host", null, null),
            ("header-with-variables", "name,title,location,Text-Size,bad id,location\nAda,Host,\"London, UK\",72,x,Paris\nBo,,,,,\n\"Cy\",\"\",\"\",9,,", null, ["location", "text-size"]),
            ("header-with-variables-null-ids", "name,title,location,Text-Size,bad id\nAda,Host,London,72,x", null, null),
            ("header-only", "name,title,location\n", null, ["location"]),
            ("header-row-unbalanced-quote-skipped", "name,title,location\n\"Ada,Host,London\nBo,Guest,Rome", null, ["location"]),
            ("header-mode-quoted-cell-trims-inside-quotes", "name,title,location\n  \" Ada \" , \" Host \",\"  spaced  \"", null, ["location"]),
            ("blank-lines-and-crlf", "\r\nAda, Host\r\n\r\n   \r\nBo, Guest\r\n", null, null),
            ("seventy-lines-capped-at-64", many, null, null),
            ("long-name-and-title", new string('n', 200) + ", " + new string('t', 300), null, null),
            ("long-cell", "name,title,location\nAda,Host," + new string('c', 5000), null, ["location"]),
            ("duplicate-names", "Ada\nAda\nAda, Again", null, null),
            ("existing-ids-kept-reordered-and-edited", "Bo, Bass\nAda Lovelace, Countess\nDee, New\nCy", existing, null),
            ("non-ascii-name", "Zoë Ç, Café", null, null),
            ("digits-only-name", "2024, Year", null, null),
            ("header-mode-tab-before-quote-is-unquoted-cell", "name,title,location\n\t\"Ada\",Host,\"x\"\n\"Bo\",\t\"Guest\",y", null, ["location"]),
            ("decomposed-accent-at-the-128-unit-cut", new string('a', 127) + "e\u0301, " + new string('b', 255) + "e\u0301", null, null),
            ("dotted-capital-i-and-kelvin-sign", "\u0130van, Host\n\u212Aelvin, Guest", null, null),
            ("empty-name-skipped", ", Title only\n\"\", Also empty\nAda", null, null),
        };
        static JsonNode Variants(IEnumerable<GraphicsPlaylistVariant> list) => JsonSerializer.SerializeToNode(list.ToArray(), Web)!;
        foreach (var (id, text, prior, ids) in cases)
        {
            var parsed = GraphicsBuilder.ParseNamesList(text, prior, ids);
            rows.Add(Row("parse-" + id, "parse",
                new JsonObject { ["text"] = text, ["existing"] = prior is null ? null : Variants(prior), ["variableIds"] = ids is null ? null : JsonSerializer.SerializeToNode(ids) },
                new JsonObject { ["variants"] = Variants(parsed) }));
        }
        var texts = new (string Id, GraphicsPlaylistVariant[] Variants)[]
        {
            ("no-cells", [Variant("a", "Ada", "Host"), Variant("b", "Bo", ""), Variant("c", "Lovelace, Ada", "Says \"hi\""), Variant("d", "Tab\there", "semi;colon")]),
            ("with-cells", [Variant("a", "Ada", "Host", ("location", "London, UK"), ("text-size", "72")), Variant("b", "Bo", "", ("text-size", "9")), Variant("c", "Cy", "Painter")]),
            ("empty", []),
        };
        foreach (var (id, variants) in texts)
            rows.Add(Row("text-" + id, "text", new JsonObject { ["variants"] = Variants(variants) },
                new JsonObject { ["text"] = GraphicsBuilder.NamesListText(variants).Replace("\r\n", "\n") }));
        foreach (var (id, name, taken) in new (string, string, string[])[] {
                     ("simple", "Ada Lovelace", []), ("taken", "Ada", ["ada", "ada-2"]), ("non-ascii", "Zoë Ç", []), ("digits-only", "2024", []),
                     ("leading-digits", "3 Body Problem", []), ("long", new string('x', 30) + " " + new string('y', 30), []), ("punctuation-only", "!!!", ["graphic"]),
                     ("dotted-capital-i", "\u0130van", []), ("kelvin-sign", "\u212Aelvin", []) })
            rows.Add(Row("new-row-id-" + id, "newRowId", new JsonObject { ["name"] = name, ["taken"] = JsonSerializer.SerializeToNode(taken) },
                new JsonObject { ["id"] = GraphicsBuilder.NewRowId(name, taken.Select(t => new GraphicsPlaylistRow { Id = t, Name = t }).ToArray()) }));
        Check("names-editor", "Names-list editor: GraphicsBuilder.ParseNamesList (with existing ids and variable columns), NamesListText (\\n line endings, plan D12) and NewRowId.", rows);
    }
}
