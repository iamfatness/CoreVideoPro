using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 5: local datasets exactly as Windows reads them (GraphicsLocalData.ReadAsync /
/// Parse / WithKey / Project). Every input goes through the real byte path: committed operator-style
/// files under contracts/graphics-shell/data/, inline texts the Mac writes to a temp file with the
/// row's extension, and generated inputs (size limits, invalid UTF-8) both sides build from a recipe.
/// CSV is Microsoft.VisualBasic.FileIO.TextFieldParser's (quoted fields, its default whitespace trim,
/// blank lines skipped); JSON keeps a number's raw text and refuses case-insensitive duplicate keys.</summary>
public sealed partial class GraphicsShellVectorTests
{
    internal static string DataPath(string relative) => ControlRepoPaths.Contract(Path.Combine("graphics-shell", relative));

    /// <summary>The bytes a generated input stands for: prefix + unit × count + suffix, or explicit base64.</summary>
    internal static byte[] GeneratedBytes(JsonObject recipe)
    {
        if (recipe["base64"] is JsonValue b64) return Convert.FromBase64String(b64.GetValue<string>());
        var text = new StringBuilder((string?)recipe["prefix"] ?? "");
        var unit = (string)recipe["unit"]!; var count = (int)recipe["count"]!;
        for (var i = 0; i < count; i++) text.Append(unit.Replace("{i}", i.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        text.Append((string?)recipe["suffix"] ?? "");
        return new UTF8Encoding(false).GetBytes(text.ToString());
    }

    internal static JsonObject DataResult(Func<GraphicsLocalData> read)
    {
        try
        {
            var data = read();
            return new JsonObject
            {
                ["columns"] = new JsonArray(data.Columns.Select(c => (JsonNode)new JsonObject { ["id"] = c.Id, ["kind"] = c.Kind }).ToArray()),
                ["rows"] = new JsonArray(data.Rows.Select(r => (JsonNode)new JsonObject { ["key"] = r.Key, ["cells"] = new JsonArray(r.Cells.Select(c => (JsonNode)c).ToArray()) }).ToArray()),
            };
        }
        catch (InvalidDataException ex) { return new JsonObject { ["error"] = ex.Message }; }
    }

    internal static GraphicsLocalData ReadBytes(byte[] bytes, string extension, string? key)
    {
        var directory = Path.Combine(Path.GetTempPath(), "cvp-data-vector-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var path = Path.Combine(directory, "input" + extension);
            File.WriteAllBytes(path, bytes);
            return GraphicsLocalData.ReadAsync(path, key).GetAwaiter().GetResult();
        }
        finally { Directory.Delete(directory, true); }
    }

    [Fact]
    public void DataVectors()
    {
        var rows = new JsonArray();
        void File(string id, string file, string? key) =>
            rows.Add(Row("parse-" + id, "read", new JsonObject { ["file"] = file, ["key"] = key },
                DataResult(() => GraphicsLocalData.ReadAsync(DataPath(file), key).GetAwaiter().GetResult())));
        void Text(string id, string text, string extension = ".csv", string? key = null) =>
            rows.Add(Row("parse-" + id, "read", new JsonObject { ["text"] = text, ["extension"] = extension, ["key"] = key },
                DataResult(() => ReadBytes(new UTF8Encoding(false).GetBytes(text), extension, key))));
        void Generated(string id, JsonObject recipe, string extension, string? key = null) =>
            rows.Add(Row("parse-" + id, "read", new JsonObject { ["generate"] = recipe.DeepClone(), ["extension"] = extension, ["key"] = key },
                DataResult(() => ReadBytes(GeneratedBytes(recipe), extension, key))));

        // Committed operator-style files (contracts/graphics-shell/data/README.md states their invariants).
        File("scores-csv-keyed", "data/scores.csv", "team");
        File("scores-csv-file-order", "data/scores.csv", null);
        File("scores-csv-key-with-spaces", "data/scores.csv", "  team ");
        File("scores-csv-missing-key", "data/scores.csv", "rank");
        File("crawl-csv", "data/crawl.csv", null);
        File("scores-json-keyed", "data/scores.json", "team");

        // CSV, TextFieldParser semantics.
        Text("csv-basic", "a,b\n1,2");
        Text("csv-crlf", "a,b\r\n1,2\r\n");
        Text("csv-cr-only", "a,b\r1,2\r3,4");
        Text("csv-whitespace-only-line", "a,b\n   \n1,2");
        Text("csv-tab-only-line", "a,b\n\t\n1,2");
        Text("csv-trailing-blank-lines", "a,b\n1,2\n\n\n  \n");
        Text("csv-leading-blank-lines", "\n\n a , b \n1,2");
        Text("csv-quoted-with-spaces", "a,b\n 1 , \" 2 \" ");
        Text("csv-space-after-closing-quote", "a,b\n\"x\" ,y");
        Text("csv-junk-after-closing-quote", "a,b\n\"x\"z,y");
        Text("csv-quote-inside-unquoted", "a,b\nx\"y,z");
        Text("csv-unterminated-quote", "a,b\n\"open,y");
        Text("csv-row-too-long", "a,b\n1,2,3");
        Text("csv-row-too-short", "a,b\n1");
        Text("csv-empty", "");
        Text("csv-only-blank-lines", "\n\n");
        Text("csv-header-only", "a,b\n");
        Text("csv-duplicate-headings", "a,a\n1,2");
        Text("csv-case-duplicate-headings", "A,a\n1,2");
        Text("csv-quoted-empty-cell", "a\n\"\"\nx");
        Text("csv-empty-cells", "a,b\n,\n1,");
        Text("csv-multiline-quoted", "a,b\n\"multi\nline\",2");
        Text("csv-crlf-inside-quotes", "a,b\r\n\"two\r\nlines\",2\r\n");
        Text("csv-quoted-escapes", "a,b\n\"Say \"\"hi\"\"\",\"\"\"\"");
        Text("csv-tabs-around-fields", "a,b\n\tx\t,\ty");
        Text("csv-nbsp-around-fields", "a,b\n\u00a0x\u00a0,y");
        Text("csv-numbers", "n\n+1\n-0.5\n.5\n5.\n1E+3\n 7 \n0x10");
        Text("csv-numbers-strict", "n\n+1\n-0.5\n.5\n5.\n1E+3\n 7 ");
        // One value per row, so each number / boolean spelling is judged on its own (double.TryParse Float + IsFinite, bool.TryParse).
        foreach (var (name, value) in new[] { ("e-no-digits", "1e"), ("e-sign-no-digits", "1e+"), ("dot", "."), ("plus-dot-five", "+.5"), ("minus-dot", "-."),
                     ("one-dot-e3", "1.e3"), ("underscore", "1_000"), ("arabic-digits", "\u0661\u0662"), ("hex", "0x1"), ("suffix-d", "1d"),
                     ("tabs-around", "\t1\t"), ("vertical-tab", "\v1"), ("inner-space", "1 2"), ("plus-minus", "+-1"), ("unicode-minus", "\u22121"),
                     ("upper-e", "1E5"), ("leading-zeros", "00012"), ("negative-zero", "-0"), ("underflow", "1e-400"), ("infinity-symbol", "\u221e"),
                     ("negative-infinity", "-Infinity"), ("nan-lower", "nan"), ("bool-spaced", " true "), ("bool-upper", "TRUE"), ("bool-t", "t"), ("bool-1", "1"),
                     ("nbsp-around-number", "\u00a01\u00a0") })
            Text("value-" + name, "v\n\"" + value.Replace("\"", "\"\"") + "\"");
        Text("csv-nan-is-text", "n\n1\nNaN");
        Text("csv-infinity-is-text", "n\n1\nInfinity");
        Text("csv-overflow-is-text", "n\n1\n1e400");
        Text("csv-thousands-is-text", "n\n\"1,000\"\n2");
        Text("csv-booleans", "b\nTrue\nfalse\n FALSE \n");
        Text("csv-yes-is-text", "b\ntrue\nyes");
        Text("csv-all-empty-column-is-text", "a,b\nx,\ny,");
        Text("csv-duplicate-keys", "k,v\na,1\na,2", key: "k");
        Text("csv-empty-key", "k,v\na,1\n,2", key: "k");
        Text("csv-whitespace-key", "k,v\na,1\n\" \",2", key: "k");
        Text("csv-long-cell", "k\n" + new string('c', 4097));
        Text("csv-cell-at-4096", "k\n" + new string('c', 4096));
        Text("csv-long-key", "k\n" + new string('k', 257), key: "k");
        Text("csv-upper-extension", "a\n1", ".CSV");
        Text("txt-extension", "a\n1", ".txt");
        Text("csv-non-ascii", "équipe,ville\nÉlans,Zürich\n日本,東京");
        Text("csv-emoji", "a\n🎙️ host");
        Text("csv-quoted-trailing-space-last-line", "a,b\n1,\"2\" ");
        Text("csv-quoted-trailing-space-mid-line", "a,b\n1,\"2\" \n3,4");
        Text("csv-quoted-then-ideographic-space", "a,b\n1,\"2\" \u3000");
        Text("csv-zero-width-space-not-trimmed", "a,b\n\u200bx\u200b,y");
        Text("csv-next-line-char-trimmed", "a,b\n\u0085x,y");
        Text("csv-blank-line-inside-quotes", "a,b\n\"one\n\n   \ntwo\",2");
        Text("csv-quoted-field-at-eof-unterminated", "a\n\"x\n");
        Text("csv-doubled-quote-then-text", "a\n\"\"x");
        Text("csv-quote-only-field", "a\n\"\"\"\"");
        Text("csv-trailing-comma", "a,b\nx,\n");
        Text("csv-trailing-comma-last-line", "a,b\nx,");
        Text("csv-line-separator-inside", "a,b\nx\u2028y,z");
        Generated("csv-33-columns", new JsonObject { ["prefix"] = "", ["unit"] = "c{i},", ["count"] = 32, ["suffix"] = "c32\n" + string.Join(",", Enumerable.Repeat("1", 33)) }, ".csv");
        Generated("csv-32-columns", new JsonObject { ["prefix"] = "", ["unit"] = "c{i},", ["count"] = 31, ["suffix"] = "c31\n" + string.Join(",", Enumerable.Repeat("1", 32)) }, ".csv");
        Generated("csv-1001-rows", new JsonObject { ["prefix"] = "a\n", ["unit"] = "r{i}\n", ["count"] = 1001 }, ".csv");
        Generated("csv-1000-rows", new JsonObject { ["prefix"] = "a\n", ["unit"] = "r{i}\n", ["count"] = 1000 }, ".csv");
        Generated("csv-over-1-mib", new JsonObject { ["prefix"] = "a\n", ["unit"] = "x", ["count"] = 1024 * 1024 }, ".csv");
        Generated("invalid-utf8", new JsonObject { ["base64"] = Convert.ToBase64String([0x61, 0x0a, 0xff, 0xfe, 0x0a]) }, ".csv");
        Generated("bom-stripped", new JsonObject { ["base64"] = Convert.ToBase64String([0xef, 0xbb, 0xbf, 0x61, 0x0a, 0x31]) }, ".csv");
        Generated("double-bom-stripped", new JsonObject { ["base64"] = Convert.ToBase64String([0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf, 0x61, 0x0a, 0x31]) }, ".csv");

        // JSON: a purpose-built scanner on the Mac (raw number text, property order, case-insensitive duplicates).
        Text("json-basic", "[{\"a\":\"x\",\"n\":1.50}]", ".json");
        Text("json-numbers-raw", "[{\"n\":1.50},{\"n\":1e3},{\"n\":-0},{\"n\":0.1E-2},{\"n\":12345678901234567890}]", ".json");
        Text("json-number-overflow", "[{\"n\":1e999}]", ".json");
        Text("json-case-duplicate-keys", "[{\"Team\":\"a\",\"team\":\"b\"}]", ".json");
        Text("json-exact-duplicate-keys", "[{\"a\":1,\"a\":2}]", ".json");
        Text("json-mixed-kinds", "[{\"a\":1},{\"a\":\"x\"}]", ".json");
        Text("json-bool-and-string", "[{\"a\":true},{\"a\":\"true\"}]", ".json");
        Text("json-null-column-is-text", "[{\"a\":null,\"b\":1},{\"a\":null,\"b\":2}]", ".json");
        Text("json-null-then-number", "[{\"a\":null},{\"a\":5}]", ".json");
        Text("json-reordered-properties", "[{\"a\":1,\"b\":\"x\"},{\"b\":\"y\",\"a\":2}]", ".json");
        Text("json-different-columns", "[{\"a\":1},{\"b\":2}]", ".json");
        Text("json-fewer-columns", "[{\"a\":1,\"b\":2},{\"a\":3}]", ".json");
        Text("json-not-an-array", "{\"a\":1}", ".json");
        Text("json-empty-array", "[]", ".json");
        Text("json-row-not-object", "[1]", ".json");
        Text("json-empty-object", "[{}]", ".json");
        Text("json-nested-object-cell", "[{\"a\":{\"b\":1}}]", ".json");
        Text("json-depth-4-array-cell", "[{\"a\":[[1]]}]", ".json");
        Text("json-depth-5", "[{\"a\":[[[1]]]}]", ".json");
        Text("json-trailing-comma", "[{\"a\":1},]", ".json");
        Text("json-comment", "[{\"a\":1}] // note", ".json");
        Text("json-trailing-data", "[{\"a\":1}] x", ".json");
        Text("json-leading-zero", "[{\"a\":01}]", ".json");
        Text("json-bad-escape", "[{\"a\":\"\\x\"}]", ".json");
        Text("json-raw-control-char", "[{\"a\":\"x\ty\"}]", ".json");
        Text("json-escapes", "[{\"a\":\"line\\nbreak\\ttab \\\"q\\\" \\\\ \\/ \\u00e9 \\ud83d\\ude00 \\u2028\"}]", ".json");
        Text("json-nbsp-whitespace", "[\u00a0{\"a\":1}]", ".json");
        Text("json-whitespace", " \r\n\t[ {\"a\" : 1 } ]\n", ".json");
        Text("json-single-quotes", "[{'a':1}]", ".json");
        Text("json-uppercase-literal", "[{\"a\":True}]", ".json");
        Text("json-keyed", "[{\"id\":\"x\",\"v\":1},{\"id\":\"y\",\"v\":2}]", ".json", "id");
        Text("json-key-from-number", "[{\"id\":1,\"v\":1},{\"id\":2,\"v\":2}]", ".json", "id");
        Text("json-33-columns", "[{" + string.Join(",", Enumerable.Range(0, 33).Select(i => $"\"c{i}\":1")) + "}]", ".json");
        Text("json-key-escaped-name", "[{\"a\\u0062\":1}]", ".json");
        Generated("json-1001-rows", new JsonObject { ["prefix"] = "[{\"a\":0}", ["unit"] = ",{\"a\":{i}}", ["count"] = 1000, ["suffix"] = "]" }, ".json");

        // WithKey and Project over the scores file read in file order.
        var scores = GraphicsLocalData.ReadAsync(DataPath("data/scores.csv"), null).GetAwaiter().GetResult();
        foreach (var key in new[] { "team", "points", "missing", "city" })
            rows.Add(Row("with-key-" + key, "withKey", new JsonObject { ["file"] = "data/scores.csv", ["key"] = key },
                DataResult(() => scores.WithKey(key))));
        var keyed = scores.WithKey("team");
        JsonObject Project(string[] columns, string? orderBy, int limit)
        {
            try { return new JsonObject { ["rows"] = JsonSerializer.SerializeToNode(keyed.Project(columns, orderBy, limit)) }; }
            catch (InvalidDataException ex) { return new JsonObject { ["error"] = ex.Message }; }
        }
        foreach (var columns in new[] { new[] { "team" }, new[] { "team", "points" }, new[] { "city", "team", "qualified", "points" } })
            foreach (var order in new string?[] { null, "points" })
                foreach (var limit in new[] { 1, 3, 12 })
                    rows.Add(Row($"project-{string.Join("+", columns)}-{order ?? "file"}-{limit}", "project",
                        new JsonObject { ["file"] = "data/scores.csv", ["key"] = "team", ["columns"] = JsonSerializer.SerializeToNode(columns), ["orderBy"] = order, ["limit"] = limit },
                        Project(columns, order, limit)));
        foreach (var (id, columns, order, limit) in new (string, string[], string?, int)[] {
                     ("nine-columns", ["team", "points", "qualified", "city", "team", "points", "qualified", "city", "team"], null, 3),
                     ("duplicate-columns", ["team", "team"], null, 3), ("limit-13", ["team"], null, 13), ("limit-0", ["team"], null, 0),
                     ("sort-text-column", ["team"], "city", 3), ("missing-column", ["rank"], null, 3), ("blank-order-is-file-order", ["team"], "  ", 3) })
            rows.Add(Row("project-refused-" + id, "project",
                new JsonObject { ["file"] = "data/scores.csv", ["key"] = "team", ["columns"] = JsonSerializer.SerializeToNode(columns), ["orderBy"] = order, ["limit"] = limit },
                Project(columns, order, limit)));
        Check("data", "Local datasets: GraphicsLocalData.ReadAsync (byte path, UTF-8, BOM, 1 MiB), CSV via TextFieldParser, JSON with raw number text, WithKey and Project.", rows);
    }
}
