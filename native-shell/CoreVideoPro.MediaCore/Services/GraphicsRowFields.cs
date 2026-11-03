using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>One text variable an operator types on an Operate playlist row.</summary>
public sealed record GraphicsRowTypeableField(string Id, string Label, int? MaxLength);

/// <summary>Spec §4: "where values come from stays on the playlist row … or typed". Which of a row's
/// template variables the Operate inspector offers as a text box, and how a typed value lands on
/// the row. Playout sends <see cref="GraphicsPlaylistRow.Fields"/> and overlays only the Sources
/// keys (name, title) on a Guest row that follows Sources, so a typed extra variable reaches air.</summary>
public static class GraphicsRowFields
{
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);
    /// <summary>Keys the inspector already handles (row name, Title line / Sources) or the template fills itself (clock, LIVE label).</summary>
    public static readonly IReadOnlyList<string> Handled = ["name", "title", "time", "label"];

    /// <summary>The declarative variables of the row's exact template pin: the installed template when the
    /// core has it, else the saved pin's manifest. A compatibility template or an empty row has none.</summary>
    public static IReadOnlyList<NativeGraphicsTemplateField> TemplateFieldsOf(GraphicsPlaylistRow row, NativeGraphicsState? state, IReadOnlyList<GraphicsSavedTemplate> pins)
    {
        var id = row.Image?.TemplateId ?? row.TemplateId; var version = row.Image?.TemplateVersion ?? row.TemplateVersion;
        if (id.Length == 0) return [];
        if (state?.Templates.FirstOrDefault(t => t.TemplateId == id && t.TemplateVersion == version) is { } installed)
            return installed.Runtime == "declarative" ? installed.Fields : [];
        if (pins.FirstOrDefault(p => p.TemplateId == id && p.TemplateVersion == version) is not { } pin ||
            pin.Definition.ValueKind != JsonValueKind.Object || !pin.Definition.TryGetProperty("layers", out _)) return [];
        try { return pin.Definition.Deserialize<NativeGraphicsTemplate>(Json)?.Fields ?? []; }
        catch (JsonException) { return []; }
    }

    /// <summary>Every free-text variable other than <see cref="Handled"/> and the row's own Title line
    /// (<paramref name="titleKey"/>). Variables with a fixed choice list are not typed.</summary>
    public static IReadOnlyList<GraphicsRowTypeableField> Typeable(GraphicsPlaylistRow row, NativeGraphicsState? state, IReadOnlyList<GraphicsSavedTemplate> pins, string? titleKey = null) =>
        TemplateFieldsOf(row, state, pins)
            .Where(f => f.Type == "string" && f.Choices.Count == 0 && !Handled.Contains(f.Id) && f.Id != titleKey && (row.DataBinding is null || !f.Id.StartsWith(GraphicsDataTable.RowPrefix, StringComparison.Ordinal)))
            .Select(f => new GraphicsRowTypeableField(f.Id, LabelOf(f), f.MaxLength)).ToArray();

    // Variables Operate offers no box for (the row name is the row's own Name; clock and LIVE label fill themselves)
    // follow the Designer instead (final re-review I4).
    private static readonly IReadOnlyList<string> NotTypedOnTheRow = ["name", "time", "label"];
    /// <summary>A re-pin from the Designer (Save to playlist again): <paramref name="pinned"/>'s fields come from the draft,
    /// which follows the new version. Text the operator typed on <paramref name="existing"/> (a non-empty string) wins for
    /// every field the new version still has; empty values and new variables take the draft's, dropped ones are gone.</summary>
    public static GraphicsPlaylistRow KeepTyped(GraphicsPlaylistRow pinned, GraphicsPlaylistRow? existing)
    {
        if (existing is null) return pinned;
        var fields = pinned.Fields.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal);
        foreach (var key in fields.Keys.ToArray())
            if (!NotTypedOnTheRow.Contains(key) && fields[key].ValueKind == JsonValueKind.String && existing.Fields.TryGetValue(key, out var typed) &&
                typed.ValueKind == JsonValueKind.String && typed.GetString() is { Length: > 0 }) fields[key] = typed.Clone();
        return pinned with { Fields = fields };
    }

    public static GraphicsPlaylistRow WithText(GraphicsPlaylistRow row, string key, string value)
    {
        var fields = row.Fields.ToDictionary(p => p.Key, p => p.Value, StringComparer.Ordinal);
        fields[key] = JsonSerializer.SerializeToElement(value);
        return row with { Fields = fields };
    }
    public static string Text(GraphicsPlaylistRow row, string key) =>
        row.Fields.TryGetValue(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    private static string LabelOf(NativeGraphicsTemplateField field) =>
        field.ExtensionData?.TryGetValue("metadata", out var metadata) == true && metadata.ValueKind == JsonValueKind.Object &&
        metadata.TryGetProperty("label", out var label) && label.ValueKind == JsonValueKind.String && label.GetString() is { Length: > 0 and <= 128 } friendly
            ? friendly : GraphicsTemplateEditor.HumanizeId(field.Id);
}
