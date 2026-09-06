using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public static class MediaCoreHandshakeRules
{
    public static void RequireCompatibleProtocol(JsonElement root)
    {
        // Absent means the supported legacy protocol. An explicitly advertised
        // incompatible major must not be treated as a successful connection.
        if (root.TryGetProperty("protocolVersion", out var version) &&
            !CoreVideoPro.MediaCore.Contracts.ProtocolVersionContract.Validate(version))
        {
            throw new InvalidOperationException("Media core protocol is incompatible. Install matching shell and core versions.");
        }
    }
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
