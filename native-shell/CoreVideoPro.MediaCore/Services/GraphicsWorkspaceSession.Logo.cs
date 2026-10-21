using System.Text.Json;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsWorkspaceSession
{
    public static bool SupportsLogo(NativeGraphicsTemplate? template) => template is not null &&
        (template.Runtime != "declarative" || template.Definition is { } definition &&
         definition.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array &&
         assets.EnumerateArray().Any(asset => asset.TryGetProperty("id", out var id) && id.GetString() == "logo"));

    public bool SetLogo(NativeGraphicsImageReference? image)
    {
        if (Busy || Draft is not { } draft || !SupportsLogo(Template)) return false;
        var fields = draft.Fields;
        if (draft.Declarative && image is not null) {
            var values = fields.EnumerateObject().ToDictionary(p => p.Name, p => p.Value.Clone());
            if (Template!.Fields.Any(f => f.Id == "logo" && f.Type == "image")) values["logo"] = JsonSerializer.SerializeToElement("logo");
            if (Template.Fields.Any(f => f.Id == "show-logo" && f.Type == "boolean")) values["show-logo"] = JsonSerializer.SerializeToElement(true);
            fields = JsonSerializer.SerializeToElement(values);
        }
        Edit(draft with { Image = image, Fields = fields, Style = image is not null ? draft.Style with { ShowLogo = true } : draft.Style });
        return true;
    }

    public async Task<bool> ImportLogoAsync(string path, CancellationToken cancellation = default)
    {
        if (Busy || ImportBusy || Draft is not { } before || !SupportsLogo(Template)) {
            ImportStatus = "Select a graphic with a logo slot before uploading a logo.";
            return false;
        }
        var selected = SelectedId;
        var image = await ImportImageAsync(path, cancellation);
        if (image is null) return false;
        if (SelectedId != selected || Draft != before || Busy) {
            ImportStatus = "Logo imported into the library. Your newer selection or edits were kept.";
            return false;
        }
        if (!SetLogo(image)) return false;
        ImportStatus = "Logo uploaded to the private draft with transparency preserved. Cue or Update explicitly applies it; Program is unchanged.";
        Changed?.Invoke(this, EventArgs.Empty);
        return true;
    }
}
