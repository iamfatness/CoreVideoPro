using System.Buffers;
using System.Globalization;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsFeedColumnMapping(string Column, string Pointer);

public sealed record GraphicsFeedMapping(string RowsPointer, GraphicsFeedColumnMapping[] Columns)
{
    internal bool Valid() => TryTokens(RowsPointer, out _) &&
        Columns is { Length: <= GraphicsLocalData.MaxColumns } &&
        Columns.All(column => column?.Column is { Length: > 0 and <= 128 } name && !string.IsNullOrWhiteSpace(name) &&
            name.Trim() == name && TryTokens(column.Pointer, out _)) &&
        Columns.Select(column => column.Column).Distinct(StringComparer.OrdinalIgnoreCase).Count() == Columns.Length;

    internal static GraphicsLocalData Normalize(string json, string keyColumn, GraphicsFeedMapping? mapping, CancellationToken cancellation)
    {
        try
        {
            using var document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 24 });
            RequireUniqueProperties(document.RootElement, cancellation);
            var rows = document.RootElement;
            if (mapping is not null)
            {
                if (!TryTokens(mapping.RowsPointer, out var path)) throw InvalidPointer();
                rows = Select(rows, path);
            }
            if (rows.ValueKind != JsonValueKind.Array || rows.GetArrayLength() is < 1 or > GraphicsLocalData.MaxRows)
                throw new InvalidDataException("The custom feed needs an array of 1–1,000 rows.");
            if (mapping?.Columns is not { Length: > 0 } columns)
                return GraphicsLocalData.Parse(rows.GetRawText(), "json", keyColumn);

            var paths = columns.Select(column => TryTokens(column.Pointer, out var tokens) ? tokens : throw InvalidPointer()).ToArray();
            var buffer = new ArrayBufferWriter<byte>();
            using var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
            writer.WriteStartArray();
            foreach (var row in rows.EnumerateArray())
            {
                cancellation.ThrowIfCancellationRequested();
                writer.WriteStartObject();
                for (var index = 0; index < columns.Length; index++)
                {
                    var value = Select(row, paths[index]);
                    if (value.ValueKind is JsonValueKind.Array or JsonValueKind.Object)
                        throw new InvalidDataException("Custom feed columns must select scalar values.");
                    writer.WritePropertyName(columns[index].Column);
                    value.WriteTo(writer);
                    if (writer.BytesCommitted + writer.BytesPending > GraphicsLocalData.ByteLimit)
                        throw new InvalidDataException("Mapped graphics feed exceeds 1 MiB.");
                }
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
            writer.Flush();
            return GraphicsLocalData.Parse(Encoding.UTF8.GetString(buffer.WrittenSpan), "json", keyColumn);
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException or ArgumentException)
        {
            throw new InvalidDataException("Invalid custom feed JSON.");
        }
    }

    private static bool TryTokens(string? pointer, out string[] tokens)
    {
        tokens = [];
        if (pointer is null || pointer.Length > 512) return false;
        if (pointer.Length == 0) return true;
        if (pointer[0] != '/') return false;
        var parts = pointer[1..].Split('/');
        if (parts.Length > 16) return false;
        foreach (var part in parts)
        {
            for (var index = 0; index < part.Length; index++)
            {
                if (part[index] != '~') continue;
                if (++index == part.Length || part[index] is not ('0' or '1')) return false;
            }
        }
        tokens = parts.Select(part => part.Replace("~1", "/", StringComparison.Ordinal)
            .Replace("~0", "~", StringComparison.Ordinal)).ToArray();
        return true;
    }

    private static JsonElement Select(JsonElement value, string[] tokens)
    {
        foreach (var token in tokens)
        {
            if (value.ValueKind == JsonValueKind.Object && value.TryGetProperty(token, out var property))
            {
                value = property;
                continue;
            }
            if (value.ValueKind == JsonValueKind.Array && token.Length > 0 && (token.Length == 1 || token[0] != '0') &&
                token.All(char.IsAsciiDigit) && int.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out var index) &&
                index < value.GetArrayLength())
            {
                value = value[index];
                continue;
            }
            throw InvalidPointer();
        }
        return value;
    }

    private static InvalidDataException InvalidPointer() => new("A custom feed pointer does not select an existing value.");

    private static void RequireUniqueProperties(JsonElement value, CancellationToken cancellation)
    {
        cancellation.ThrowIfCancellationRequested();
        if (value.ValueKind == JsonValueKind.Object)
        {
            var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in value.EnumerateObject())
            {
                if (!names.Add(property.Name)) throw new InvalidDataException("Duplicate custom feed JSON property.");
                RequireUniqueProperties(property.Value, cancellation);
            }
        }
        else if (value.ValueKind == JsonValueKind.Array)
            foreach (var child in value.EnumerateArray()) RequireUniqueProperties(child, cancellation);
    }
}
