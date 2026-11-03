using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static partial class GraphicsBuilder
{
    private static string?[]? NameListColumns(string[] lines, IReadOnlyCollection<string>? variableIds)
    {
        var header = lines.FirstOrDefault(line => !string.IsNullOrWhiteSpace(line));
        if (header is null || ReadNameListCells(header.Trim()) is not { Count: > 2 } cells ||
            !cells[0].Value.Equals("name", StringComparison.OrdinalIgnoreCase) ||
            !cells[1].Value.Equals("title", StringComparison.OrdinalIgnoreCase))
            return null;

        var allowed = variableIds?.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var used = new HashSet<string>(["name", "title"], StringComparer.OrdinalIgnoreCase);
        var columns = new string?[cells.Count];
        for (var i = 2; i < cells.Count && used.Count < 32; i++)
        {
            var id = cells[i].Value.ToLowerInvariant();
            if (GraphicsPlaylistRow.ValidId(id) && (allowed is null || allowed.Contains(id)) && used.Add(id))
                columns[i] = id;
        }
        return columns;
    }

    private static List<(string Value, bool Quoted)>? ReadNameListCells(string line)
    {
        var cells = new List<(string Value, bool Quoted)>();
        while (true)
        {
            var cell = line.TrimStart(' ');
            if (cell.StartsWith('"'))
            {
                var close = ClosingQuote(cell);
                if (close < 0) return null;
                cells.Add((Unquote(cell[..(close + 1)]), true));
                line = cell[(close + 1)..].TrimStart(' ');
                if (line.Length == 0) return cells;
                if (line[0] != ',') return null;
            }
            else
            {
                var comma = line.IndexOf(',');
                if (comma < 0)
                {
                    cells.Add((line.Trim(), false));
                    return cells;
                }
                cells.Add((line[..comma].Trim(), false));
                line = line[comma..];
            }
            line = line[1..];
        }
    }

    private static string WriteNameListCells(IReadOnlyList<GraphicsPlaylistVariant> variants)
    {
        var columns = variants.SelectMany(variant => variant.Cells.Keys).Distinct(StringComparer.Ordinal).ToArray();
        static string Quote(string value) => "\"" + value.Replace("\"", "\"\"") + "\"";
        static string Escape(string value) => value.Contains(',') || value.Contains('"') ? Quote(value) : value;
        if (columns.Length == 0)
            return string.Join(Environment.NewLine, variants.Select(variant =>
                variant.Title.Length == 0 ? Escape(variant.Name) : $"{Escape(variant.Name)}, {Escape(variant.Title)}"));

        var lines = new List<string> { "name,title," + string.Join(",", columns) };
        foreach (var variant in variants)
        {
            var values = new List<string> { Escape(variant.Name), Escape(variant.Title) };
            values.AddRange(columns.Select(column => variant.Cells.TryGetValue(column, out var value) ? Quote(value) : ""));
            lines.Add(string.Join(",", values));
        }
        return string.Join(Environment.NewLine, lines);
    }
}
