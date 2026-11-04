using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsDataBinding(string SourceId, string? KeyColumn, string[] Columns, string? SortColumn, int Limit, long Revision)
{
    public bool Valid() => SourceId is not null && GraphicsPlaylistRow.ValidId(SourceId) &&
        Columns is { Length: > 0 and <= GraphicsLocalData.MaxVisibleColumns } &&
        Columns.All(column => column is { Length: > 0 and <= 128 }) &&
        Columns.Distinct(StringComparer.Ordinal).Count() == Columns.Length &&
        KeyColumn is null or { Length: > 0 and <= 128 } &&
        SortColumn is null or { Length: > 0 and <= 128 } &&
        (SortColumn is null || Columns.Length < GraphicsLocalData.MaxVisibleColumns) &&
        Limit is >= 1 and <= GraphicsLocalData.MaxVisibleRows && Revision > 0;
}

public static class GraphicsDataTable
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    private static readonly JsonSerializerOptions CellsJson = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
    private const string ManifestTypeKey = "type";
    public const string RowPrefix = "table-row-";
    public static string FieldId(int index) => RowPrefix + (index + 1).ToString(System.Globalization.CultureInfo.InvariantCulture);

    public static Dictionary<string, JsonElement> Fields(GraphicsLocalData data, GraphicsDataBinding binding)
    {
        if (!binding.Valid()) throw new InvalidDataException("Choose valid columns, row limit and leaderboard sort.");
        var rows = data.Project(binding.Columns, binding.SortColumn, binding.Limit);
        var fields = new Dictionary<string, JsonElement>(StringComparer.Ordinal);
        for (var index = 0; index < binding.Limit; index++)
        {
            var row = index < rows.Length ? rows[index] : null;
            if (row is not null && binding.SortColumn is not null)
                row = [(index + 1).ToString(System.Globalization.CultureInfo.InvariantCulture), .. row];
            var encoded = row is null ? "" : JsonSerializer.Serialize(row, CellsJson);
            if (Encoding.UTF8.GetByteCount(encoded) > 4096) throw new InvalidDataException("A displayed row is too long. Shorten its values or select fewer columns.");
            fields.Add(FieldId(index), JsonSerializer.SerializeToElement(encoded));
        }
        return fields;
    }

    public static JsonObject Manifest(string id, string name, GraphicsDataBinding binding, GraphicsLocalData data)
    {
        var fields = Fields(data, binding);
        var seed = GraphicsDesignSeeds.Manifest("title", id, 1, name);
        seed["bounds"] = new JsonObject { ["width"] = 1440, ["height"] = (binding.Limit + 1) * 56 };
        seed["fields"] = new JsonArray(fields.Select(field => (JsonNode)new JsonObject
        {
            ["id"] = field.Key, [ManifestTypeKey] = "string", ["maxLength"] = 4096,
            ["default"] = field.Value.GetString()
        }).ToArray());
        var columns = binding.SortColumn is null ? binding.Columns : ["Rank", .. binding.Columns];
        seed["layers"] = new JsonArray(new JsonObject
        {
            ["id"] = "data-table", [ManifestTypeKey] = "table", ["x"] = 0, ["y"] = 0, ["width"] = 1440,
            ["height"] = (binding.Limit + 1) * 56, ["rowHeight"] = 56, ["fontSize"] = 29,
            ["font"] = new JsonObject { ["token"] = "brand.fontBody" },
            ["color"] = new JsonObject { ["token"] = "brand.onPrimary" },
            ["headerFill"] = new JsonObject { ["token"] = "brand.primary" },
            ["rowFill"] = "#18232D", ["alternateFill"] = "#243440",
            ["columns"] = new JsonArray(columns.Select((column, index) => (JsonNode)new JsonObject
            {
                ["heading"] = column, ["weight"] = binding.SortColumn is not null && index == 0 ? 0.4 : 1,
                ["align"] = column == binding.SortColumn || (binding.SortColumn is not null && index == 0) ? "right" : "left"
            }).ToArray()),
            ["rows"] = new JsonArray(fields.Keys.Select(field => (JsonNode)new JsonObject { ["field"] = field }).ToArray()),
            ["metadata"] = new JsonObject { ["label"] = "Data table" }
        });
        seed["metadata"] = new JsonObject { ["dataBinding"] = JsonSerializer.SerializeToNode(binding, Json) };
        return seed;
    }

    public static GraphicsDataBinding? Binding(JsonElement definition)
    {
        if (definition.ValueKind != JsonValueKind.Object || !definition.TryGetProperty("metadata", out var metadata) ||
            metadata.ValueKind != JsonValueKind.Object || !metadata.TryGetProperty("dataBinding", out var data)) return null;
        try { return data.Deserialize<GraphicsDataBinding>(Json) is { } binding && binding.Valid() ? binding : null; }
        catch (JsonException) { return null; }
    }
}
