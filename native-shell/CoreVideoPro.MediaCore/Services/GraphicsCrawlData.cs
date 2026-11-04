using System.Globalization;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsCrawlOptions(double Speed = 120, double Gap = 48, string Direction = "left", string Mode = "loop")
{
    public bool Valid() => Speed is >= 1 and <= 1200 && Gap is >= 0 and <= 512 &&
        Direction is "left" or "right" && Mode is "loop" or "single";
}

public static class GraphicsCrawlData
{
    public const int MaxItems = 64;
    public const string ItemsField = "crawl-items";
    public const string RevisionField = "crawl-revision";
    private const string ManifestTypeKey = "type";
    private static readonly JsonSerializerOptions ItemJson = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    public static string[] Items(GraphicsLocalData data, string column, int limit)
    {
        int index = Array.FindIndex(data.Columns, item => item.Id == column && item.Kind == "text");
        if (index < 0) throw new InvalidDataException("Choose a text column for the crawl.");
        return data.Rows.Select(row => row.Cells[index].Trim()).Where(value => value.Length > 0).Take(limit).ToArray();
    }

    internal static Dictionary<string, JsonElement> Fields(GraphicsLocalData data, GraphicsDataBinding binding)
    {
        var items = Items(data, binding.Columns[0], binding.Limit);
        if (items.Length == 0 || items.Any(item => item.IndexOfAny(['\r', '\n', '\t', '\0']) >= 0))
            throw new InvalidDataException("The crawl needs nonempty, single-line text.");
        var encoded = JsonSerializer.Serialize(items, ItemJson);
        if (Encoding.UTF8.GetByteCount(encoded) > 4096)
            throw new InvalidDataException("The crawl text is too long. Shorten its items or choose a smaller limit.");
        return new(StringComparer.Ordinal)
        {
            [ItemsField] = JsonSerializer.SerializeToElement(encoded),
            [RevisionField] = JsonSerializer.SerializeToElement(binding.Revision.ToString(CultureInfo.InvariantCulture))
        };
    }

    internal static JsonObject Manifest(string id, string name, GraphicsDataBinding binding, GraphicsLocalData data)
    {
        var options = binding.Crawl!;
        var fields = Fields(data, binding);
        var seed = GraphicsDesignSeeds.Manifest("title", id, 1, name);
        seed["bounds"] = new JsonObject { ["width"] = 1440, ["height"] = 64 };
        seed["fields"] = new JsonArray(fields.Select(field => (JsonNode)new JsonObject
        {
            ["id"] = field.Key, [ManifestTypeKey] = "string", ["maxLength"] = 4096, ["default"] = field.Value.GetString()
        }).ToArray());
        seed["layers"] = new JsonArray(new JsonObject
        {
            ["id"] = "data-crawl", [ManifestTypeKey] = "crawl", ["x"] = 0, ["y"] = 0, ["width"] = 1440, ["height"] = 64,
            ["fontSize"] = 34, ["font"] = new JsonObject { ["token"] = "brand.fontBody" },
            ["color"] = new JsonObject { ["token"] = "brand.onPrimary" }, ["items"] = new JsonObject { ["field"] = ItemsField },
            ["speed"] = options.Speed, ["gap"] = options.Gap, ["direction"] = options.Direction, ["mode"] = options.Mode,
            ["metadata"] = new JsonObject { ["label"] = "Data crawl" }
        });
        return seed;
    }
}
