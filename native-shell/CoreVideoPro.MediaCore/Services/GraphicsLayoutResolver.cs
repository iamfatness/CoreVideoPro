using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Contracts;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>Pure preflight for the row inputs that a data-aware layout will prepare.</summary>
internal static class GraphicsLayoutResolver
{
    internal sealed record ResolvedRow(
        GraphicsPlaylistRow Row,
        SourceInstanceIdentity? Identity);

    public static ResolvedRow Resolve(
        GraphicsShowDocument document, NativeGraphicsState state, ShotLayout layout, GraphicsPlaylistEntry entry)
    {
        var row = document.Playlist.Single(row => row.Id == entry.RowId);
        var templateId = row.Image?.TemplateId ?? row.TemplateId;
        var version = row.Image?.TemplateVersion ?? row.TemplateVersion;
        var template = state.Templates.FirstOrDefault(item => item.TemplateId == templateId && item.TemplateVersion == version);
        var pin = document.Templates.FirstOrDefault(item => item.TemplateId == templateId && item.TemplateVersion == version);
        if (templateId.Length == 0 || (template is null && pin is null))
            throw new InvalidDataException($"{row.Name}: the selected template is unavailable.");
        if (template is not null && pin is not null && template.ManifestHash != pin.ManifestHash)
            throw new InvalidDataException($"{row.Name}: the installed template differs from its saved pin.");

        var identity = state.Sources.FirstOrDefault(source => source.VideoOn && source.Identity?.SourceId == entry.SourceId)?.Identity;
        if (entry.SourceId.Length > 0 && !entry.IsVariant && identity is null)
            throw new InvalidDataException($"{row.Name}: the selected source is unavailable.");

        var descriptors = GraphicsRowFields.TemplateFieldsOf(row, state, document.Templates);
        var declarative = template?.Runtime == "declarative" ||
            (template is null && pin!.Definition.ValueKind == JsonValueKind.Object && pin.Definition.TryGetProperty("layers", out _));
        var values = CueFields(row, entry, declarative, descriptors, document.SourceColumns);
        var settings = layout.RowSettings.GetValueOrDefault(row.Id);
        if (settings?.DataPin is { } dataPin)
            ApplyDataPin(document, row, dataPin, values);

        var overrides = settings?.Overrides;
        if (overrides is { Count: > 0 })
        {
            foreach (var (id, value) in overrides)
            {
                var field = descriptors.FirstOrDefault(item => item.Id == id);
                if (GraphicsDataTable.IsDataField(id) || id is "time" or "label" ||
                    field is null || field.Type == "image" || !GraphicsCommandRequest.ValidField(value, field) ||
                    (field.Type == "color" && Encoding.UTF8.GetByteCount(value.GetString()!) > (field.MaxLength ?? 128)))
                    throw new InvalidDataException($"{row.Name}: '{id}' is unavailable or the value does not match the selected template.");
                values[id] = value.Clone();
            }
        }
        return new(row with { Fields = values }, identity);
    }

    internal static Dictionary<string, JsonElement> CueFields(GraphicsPlaylistRow row, GraphicsPlaylistEntry entry,
        bool declarative, IReadOnlyList<NativeGraphicsTemplateField> descriptors, IReadOnlyList<GraphicsSourceColumn> sourceColumns)
    {
        var values = row.Fields.ToDictionary(pair => pair.Key, pair => pair.Value.Clone(), StringComparer.Ordinal);
        if (entry.IsVariant || (row.FollowSources && entry.SourceId.Length > 0))
            foreach (var (key, value) in entry.Bindings)
                if (declarative ? descriptors.Any(field => field.Id == key && field.Type == "string") : key is "name" or "title")
                    values[key] = JsonSerializer.SerializeToElement(value);
        if (declarative)
            GraphicsMappedValues.Apply(row, entry, descriptors, sourceColumns, values);
        foreach (var key in values.Keys.Where(key => declarative ? descriptors.All(field => field.Id != key) : key is not ("name" or "title")).ToArray())
            values.Remove(key);
        return values;
    }

    private static void ApplyDataPin(GraphicsShowDocument document, GraphicsPlaylistRow row, ShotLayoutDataPin pin,
        Dictionary<string, JsonElement> values)
    {
        if (row.DataBinding is not { } binding || binding.SourceId != pin.SourceId)
            throw new InvalidDataException($"{row.Name}: the selected data source changed.");
        if (binding.Revision == pin.Revision) return;
        var source = document.DataSources.FirstOrDefault(item => item.Id == pin.SourceId && item.Revision == pin.Revision)
            ?? throw new InvalidDataException($"{row.Name}: data revision {pin.Revision} is unavailable.");
        try
        {
            foreach (var (id, value) in GraphicsDataTable.Fields(source.Data, binding with { Revision = pin.Revision }))
                values[id] = value;
        }
        catch (InvalidDataException ex)
        {
            throw new InvalidDataException($"{row.Name}: {ex.Message}", ex);
        }
    }
}
