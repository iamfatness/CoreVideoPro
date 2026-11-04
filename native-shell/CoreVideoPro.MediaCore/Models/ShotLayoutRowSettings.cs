using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace CoreVideoPro.MediaCore.Models;

public sealed record ShotLayoutDataPin(string SourceId, long Revision)
{
    [JsonIgnore]
    public bool IsValid => SourceId is not null && GraphicsPlaylistRow.ValidId(SourceId) && Revision > 0;
}

public sealed record ShotLayoutRowSettings
{
    public ShotLayoutDataPin? DataPin { get; init; }

    [JsonConverter(typeof(ShotLayoutUniqueMapConverter<JsonElement>))]
    public IReadOnlyDictionary<string, JsonElement> Overrides { get; init; } =
        new Dictionary<string, JsonElement>(StringComparer.Ordinal);

    [JsonIgnore]
    public bool IsValid => (DataPin is null || DataPin.IsValid) &&
        Overrides is { Count: <= 32 } && (DataPin is not null || Overrides.Count > 0) &&
        Overrides.All(pair => GraphicsPlaylistRow.ValidVariableId(pair.Key) && IsScalar(pair.Value));

    private static bool IsScalar(JsonElement value) => value.ValueKind switch
    {
        JsonValueKind.True or JsonValueKind.False => true,
        JsonValueKind.Number => value.TryGetDouble(out var number) && double.IsFinite(number),
        JsonValueKind.String => Encoding.UTF8.GetByteCount(value.GetString()!) <= 4096,
        _ => false
    };
}

internal sealed class ShotLayoutUniqueMapConverter<T> : JsonConverter<IReadOnlyDictionary<string, T>>
{
    public override IReadOnlyDictionary<string, T> Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        using var document = JsonDocument.ParseValue(ref reader);
        if (document.RootElement.ValueKind != JsonValueKind.Object)
            throw new JsonException("Expected a Shot Layout settings object.");

        var values = new Dictionary<string, T>(StringComparer.Ordinal);
        foreach (var property in document.RootElement.EnumerateObject())
            if (values.ContainsKey(property.Name) || property.Value.Deserialize<T>(options) is not { } value)
                throw new JsonException("A Shot Layout settings key is duplicated or has no value.");
            else
                values.Add(property.Name, value);
        return values;
    }

    public override void Write(Utf8JsonWriter writer, IReadOnlyDictionary<string, T> value, JsonSerializerOptions options)
    {
        writer.WriteStartObject();
        foreach (var pair in value)
        {
            writer.WritePropertyName(pair.Key);
            JsonSerializer.Serialize(writer, pair.Value, options);
        }
        writer.WriteEndObject();
    }
}
