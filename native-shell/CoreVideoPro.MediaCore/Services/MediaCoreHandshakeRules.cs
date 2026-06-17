using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public static class MediaCoreHandshakeRules
{
    public static bool IsUnsolicitedBootstrapHandshake(JsonElement root)
    {
        if (!root.TryGetProperty("id", out var idElement) ||
            idElement.ValueKind != JsonValueKind.String ||
            idElement.GetString() != "handshake")
        {
            return false;
        }

        return root.TryGetProperty("type", out var typeElement) &&
               typeElement.GetString() == "handshake" &&
               root.TryGetProperty("ok", out var okElement) &&
               okElement.GetBoolean();
    }
}