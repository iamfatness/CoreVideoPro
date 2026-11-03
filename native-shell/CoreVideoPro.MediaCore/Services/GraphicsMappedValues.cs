using System.Globalization;
using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class GraphicsMappedValues
{
    public static bool Eligible(NativeGraphicsTemplateField field) =>
        !GraphicsRowFields.Handled.Contains(field.Id) && field.Type is "string" or "number" or "color" or "boolean";

    public static void Apply(GraphicsPlaylistRow row, GraphicsPlaylistEntry entry,
        IReadOnlyList<NativeGraphicsTemplateField> fields, IReadOnlyList<GraphicsSourceColumn> columns,
        IDictionary<string, JsonElement> values)
    {
        var variant = entry.IsVariant ? row.Variants.FirstOrDefault(v => GraphicsPlaylistEntry.VariantPrefix + v.Id == entry.SourceId) : null;
        foreach (var field in fields.Where(Eligible))
        {
            string? cell = variant?.Cells.GetValueOrDefault(field.Id);
            if (!entry.IsVariant && row.FollowSources && entry.SlotNumber is { } slot &&
                row.SourceMappings.TryGetValue(field.Id, out var columnId))
                cell = columns.FirstOrDefault(column => column.Id == columnId)?.SlotValues.GetValueOrDefault(slot);
            if (cell is not null && ConvertCell(field, cell) is { } value)
                values[field.Id] = value;
        }
    }

    private static JsonElement? ConvertCell(NativeGraphicsTemplateField field, string cell)
    {
        JsonElement? value = field.Type switch
        {
            "string" when (!field.Required || cell.Length > 0) && Encoding.UTF8.GetByteCount(cell) <= (field.MaxLength ?? 512) => JsonSerializer.SerializeToElement(cell),
            "boolean" when bool.TryParse(cell, out var enabled) => JsonSerializer.SerializeToElement(enabled),
            "number" when double.TryParse(cell, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) &&
                double.IsFinite(number) && number >= (field.Minimum ?? -1e9) &&
                number <= (field.Maximum ?? 1e9) => JsonSerializer.SerializeToElement(number),
            "color" when cell.Length is 7 or 9 && cell.Length <= (field.MaxLength ?? 128) && cell[0] == '#' && cell.Skip(1).All(Uri.IsHexDigit) =>
                JsonSerializer.SerializeToElement(cell),
            _ => null
        };
        return value is { } candidate && (field.Choices.Count == 0 || field.Choices.Any(choice => JsonElement.DeepEquals(choice, candidate)))
            ? candidate : null;
    }
}
