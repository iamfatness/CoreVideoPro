using System.Text.Json;
using System.Globalization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public static class GradePreviewProtocol
{
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web);

    public static GradePreviewObservation? Parse(string line)
    {
        try
        {
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || root.TryGetProperty("id", out _) ||
                !root.TryGetProperty("type", out var type) || type.ValueKind != JsonValueKind.String ||
                type.GetString() != "grade-preview" ||
                !root.TryGetProperty("revision", out var revision) || revision.ValueKind != JsonValueKind.Number || !revision.TryGetInt64(out _)) return null;
            var value = root.Deserialize<GradePreviewObservation>(Options);
            if (value is null || string.IsNullOrWhiteSpace(value.InstanceId) || value.InstanceId.Length > 64 ||
                string.IsNullOrWhiteSpace(value.SourceId) || value.SourceId.Length > 256 ||
                value.Revision is < 0 or > int.MaxValue ||
                value.Status is not ("ready" or "preparing" or "held" or "unavailable" or "stale")) return null;
            if ((value.Status is "ready" or "held" or "stale") &&
                (value.Texture is not { Width: > 0, Height: > 0 } texture ||
                 texture.Width > 8192 || texture.Height > 8192 || string.IsNullOrWhiteSpace(texture.SharedHandleHex) ||
                 texture.Format != "B8G8R8A8_UNORM" ||
                 !ulong.TryParse(texture.SharedHandleHex.Replace("0x", "", StringComparison.OrdinalIgnoreCase),
                     NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var handle) || handle == 0)) return null;
            return value;
        }
        catch (JsonException) { return null; }
    }
}
