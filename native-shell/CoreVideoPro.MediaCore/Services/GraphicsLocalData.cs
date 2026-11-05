using System.Globalization;
using System.Text;
using System.Text.Json;
using Microsoft.VisualBasic.FileIO;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsDataColumn(string Id, string Kind);
public sealed record GraphicsDataRow(string Key, string[] Cells);
public sealed record GraphicsLocalData(GraphicsDataColumn[] Columns, GraphicsDataRow[] Rows)
{
    public const int ByteLimit = 1024 * 1024;
    public const int MaxRows = 1000;
    public const int MaxColumns = 32;
    public const int MaxVisibleRows = 12;
    public const int MaxVisibleColumns = 8;

    internal bool HasSameRows(GraphicsLocalData other) => Rows.Length == other.Rows.Length &&
        Rows.Zip(other.Rows).All(pair => pair.First.Key == pair.Second.Key && pair.First.Cells.SequenceEqual(pair.Second.Cells));

    public static async Task<GraphicsLocalData> ReadAsync(string path, string? key, CancellationToken cancellation = default)
    {
        await using var input = File.OpenRead(path);
        if (input.Length > ByteLimit) throw new InvalidDataException("Data files can contain at most 1 MiB.");
        var bytes = new byte[ByteLimit + 1];
        var length = await input.ReadAtLeastAsync(bytes, bytes.Length, false, cancellation);
        if (length > ByteLimit) throw new InvalidDataException("Data files can contain at most 1 MiB.");
        try { return Parse(new UTF8Encoding(false, true).GetString(bytes, 0, length).TrimStart('\uFEFF'), Path.GetExtension(path).TrimStart('.'), key); }
        catch (DecoderFallbackException ex) { throw new InvalidDataException("Use a UTF-8 CSV or JSON file.", ex); }
    }

    public static GraphicsLocalData Parse(string text, string format, string? key)
    {
        if (Encoding.UTF8.GetByteCount(text) > ByteLimit) throw new InvalidDataException("Data files can contain at most 1 MiB.");
        try
        {
            var (headings, cells, kinds) = format.ToLowerInvariant() switch
            {
                "csv" => ReadCsv(text),
                "json" => ReadJson(text),
                _ => throw new InvalidDataException("Choose a CSV or JSON file.")
            };
            var keyIndex = string.IsNullOrWhiteSpace(key) ? -1 : Array.IndexOf(headings, key.Trim());
            if (!string.IsNullOrWhiteSpace(key) && keyIndex < 0) throw new InvalidDataException("The selected row key is missing.");
            var rows = cells.Select((row, index) => new GraphicsDataRow(
                keyIndex < 0 ? (index + 1).ToString("D4", CultureInfo.InvariantCulture) : row[keyIndex], row)).ToArray();
            var data = new GraphicsLocalData(headings.Select((heading, index) => new GraphicsDataColumn(heading, kinds[index])).ToArray(), rows);
            if (!data.Valid()) throw new InvalidDataException("Rows need unique nonempty keys, consistent columns, and bounded values.");
            return data;
        }
        catch (Exception ex) when (ex is MalformedLineException or JsonException)
        { throw new InvalidDataException("The data file is malformed.", ex); }
    }

    public GraphicsLocalData WithKey(string? column)
    {
        if (column is null) return this;
        var index = IndexOf(column);
        var keyed = this with { Rows = Rows.Select(row => row with { Key = row.Cells[index] }).ToArray() };
        if (!keyed.Valid()) throw new InvalidDataException("The selected key needs a unique nonempty value in each row.");
        return keyed;
    }

    public bool Valid() =>
        Columns is { Length: > 0 and <= MaxColumns } &&
        Columns.All(c => c is not null && c.Id is { Length: > 0 and <= 128 } && c.Kind is "text" or "number" or "boolean") &&
        Columns.Select(c => c.Id).Distinct(StringComparer.OrdinalIgnoreCase).Count() == Columns.Length &&
        Rows is { Length: <= MaxRows } &&
        Rows.All(r => r is not null && r.Key is { Length: > 0 and <= 256 } && !string.IsNullOrWhiteSpace(r.Key) &&
            r.Cells is not null && r.Cells.Length == Columns.Length &&
            r.Cells.Select((value, index) => value is { Length: <= 4096 } &&
                (value.Length == 0 || Columns[index].Kind switch
                {
                    "number" => Number(value, out _),
                    "boolean" => bool.TryParse(value, out _),
                    _ => true
                })).All(valid => valid)) &&
        Rows.Select(r => r.Key).Distinct(StringComparer.Ordinal).Count() == Rows.Length &&
        Rows.Sum(r => r.Cells.Sum(Encoding.UTF8.GetByteCount)) <= ByteLimit;

    public string[][] Project(IReadOnlyList<string> columns, string? orderBy, int limit)
    {
        if (columns.Count is < 1 or > MaxVisibleColumns || limit is < 1 or > MaxVisibleRows ||
            columns.Distinct(StringComparer.Ordinal).Count() != columns.Count)
            throw new InvalidDataException("Select up to eight distinct columns and twelve rows.");
        var selected = columns.Select(IndexOf).ToArray();
        IEnumerable<GraphicsDataRow> rows = Rows;
        if (!string.IsNullOrWhiteSpace(orderBy))
        {
            var sort = IndexOf(orderBy);
            if (Columns[sort].Kind != "number") throw new InvalidDataException("Leaderboard sorting needs a numeric column.");
            rows = rows.OrderByDescending(row => Number(row.Cells[sort], out var number) ? number : double.NegativeInfinity)
                .ThenBy(row => row.Key, StringComparer.Ordinal);
        }
        return rows.Take(limit).Select(row => selected.Select(i => row.Cells[i]).ToArray()).ToArray();
    }

    private int IndexOf(string id)
    {
        var index = Array.FindIndex(Columns, c => c.Id == id);
        return index >= 0 ? index : throw new InvalidDataException($"Data column '{id}' is missing.");
    }

    private static bool Number(string value, out double number) =>
        double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out number) && double.IsFinite(number);

    private static (string[], List<string[]>, string[]) ReadCsv(string text)
    {
        using var parser = new TextFieldParser(new StringReader(text)) { HasFieldsEnclosedInQuotes = true };
        parser.SetDelimiters(",");
        var headings = parser.ReadFields() ?? throw new InvalidDataException("CSV needs a header row.");
        if (headings.Length is < 1 or > MaxColumns) throw new InvalidDataException("Use at most 32 data columns.");
        var rows = new List<string[]>();
        while (!parser.EndOfData)
        {
            if (rows.Count == MaxRows) throw new InvalidDataException("Use at most 1,000 data rows.");
            var row = parser.ReadFields()!;
            if (row.Length != headings.Length) throw new InvalidDataException("Every CSV row must match the header.");
            rows.Add(row);
        }
        return (headings, rows, headings.Select((heading, i) =>
        {
            var values = rows.Select(row => row[i]).Where(value => value.Length > 0).ToArray();
            if (values.Length == 0) return "text";
            if (values.All(value => Number(value, out _))) return "number";
            return values.All(value => bool.TryParse(value, out _)) ? "boolean" : "text";
        }).ToArray());
    }

    private static (string[], List<string[]>, string[]) ReadJson(string text)
    {
        using var document = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = 4 });
        if (document.RootElement.ValueKind != JsonValueKind.Array || document.RootElement.GetArrayLength() is < 1 or > MaxRows)
            throw new InvalidDataException("JSON needs an array of 1–1,000 row objects.");
        string[]? headings = null;
        string?[]? kinds = null;
        var rows = new List<string[]>();
        foreach (var element in document.RootElement.EnumerateArray())
        {
            if (element.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Every JSON row must be an object.");
            var properties = element.EnumerateObject().ToArray();
            if (properties.Length is < 1 or > MaxColumns ||
                properties.Select(p => p.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != properties.Length)
                throw new InvalidDataException("Use distinct JSON columns and at most 32 per row.");
            headings ??= properties.Select(p => p.Name).ToArray();
            kinds ??= new string?[headings.Length];
            if (properties.Length != headings.Length) throw new InvalidDataException("Every JSON row must have the same columns.");
            var cells = new string[headings.Length];
            foreach (var property in properties)
            {
                var index = Array.IndexOf(headings, property.Name);
                if (index < 0) throw new InvalidDataException("Every JSON row must have the same columns.");
                var (value, kind) = property.Value.ValueKind switch
                {
                    JsonValueKind.Null => ("", (string?)null),
                    JsonValueKind.String => (property.Value.GetString()!, "text"),
                    JsonValueKind.Number when Number(property.Value.GetRawText(), out _) => (property.Value.GetRawText(), "number"),
                    JsonValueKind.True or JsonValueKind.False => (property.Value.GetRawText(), "boolean"),
                    _ => throw new InvalidDataException("JSON cells must be text, numbers, booleans or null.")
                };
                if (kind is not null && kinds[index] is not null && kinds[index] != kind)
                    throw new InvalidDataException("Each JSON column must have a consistent type.");
                kinds[index] ??= kind;
                cells[index] = value;
            }
            rows.Add(cells);
        }
        return (headings!, rows, kinds!.Select(kind => kind ?? "text").ToArray());
    }
}
