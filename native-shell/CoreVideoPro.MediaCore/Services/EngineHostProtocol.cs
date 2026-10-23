using System.Buffers.Binary;
using System.IO.Pipes;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

// Control only. GPU handles remain native resources; no media payload is copied here.
public sealed record EngineHostRequest(string Operation, string ClientId, string? Epoch = null,
    long Fence = 0, long Revision = 0, string? CommandId = null, JsonElement? Payload = null,
    int Version = EngineHostWire.Version);
public sealed record EngineHostReply(bool Ok, string Epoch, long Fence, long Revision,
    bool ReadOnly, JsonElement? Payload = null, string? Error = null, int CorePid = 0,
    string[]? Events = null, int Version = EngineHostWire.Version);

public static class EngineHostWire
{
    public const int Version = 1;
    public const int MaxBytes = 8 * 1024 * 1024;
    public const int MaxUrgentBytes = 64 * 1024;
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    public static async Task<T> ReadAsync<T>(Stream stream, CancellationToken cancellationToken, int maxBytes = MaxBytes)
    {
        var header = new byte[4];
        await stream.ReadExactlyAsync(header, cancellationToken).ConfigureAwait(false);
        var length = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (length <= 0 || length > maxBytes) throw new IOException("Engine host message exceeds its byte budget.");
        var bytes = new byte[length];
        await stream.ReadExactlyAsync(bytes, cancellationToken).ConfigureAwait(false);
        return JsonSerializer.Deserialize<T>(bytes, Json) ?? throw new IOException("Empty engine host message.");
    }

    public static async Task WriteAsync<T>(Stream stream, T message, CancellationToken cancellationToken)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(message, Json);
        if (bytes.Length > MaxBytes) throw new IOException("Engine host message exceeds its byte budget.");
        var header = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(header, bytes.Length);
        await stream.WriteAsync(header, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    public static string? ResourceKey(JsonElement resource)
    {
        if (!resource.TryGetProperty("type", out var kind)) return null;
        var type = kind.GetString();
        if (type is not ("program-shared-texture" or "preview-shared-texture" or
            "multiview-shared-texture" or "participant-shared-texture")) return null;
        if (type != "participant-shared-texture") return type;
        return resource.TryGetProperty("texture", out var texture) && texture.TryGetProperty("participantId", out var id)
            ? type + ":" + id.GetString() : null;
    }

    public static bool IsUrgentType(string? type) => type is "stop-recording-session" or "stop-encoder-session" or
        "stop-program-output" or "zoom-leave" or "zoom-cancel" or "zoom-stop-capture";

    // Never let a mixed batch smuggle ordinary work into reserved capacity.
    public static bool IsUrgent(JsonElement payload)
    {
        if (payload.ValueKind != JsonValueKind.Object || !payload.TryGetProperty("type", out var type) || type.ValueKind != JsonValueKind.String) return false;
        if (IsUrgentType(type.GetString())) return true;
        return type.GetString() is "media-core-sync" or "native-media-core-sync" &&
            payload.TryGetProperty("commands", out var commands) && commands.ValueKind == JsonValueKind.Array &&
            commands.GetArrayLength() > 0 && commands.EnumerateArray().All(IsUrgent);
    }

    public static bool UsesReservedLane(EngineHostRequest request) =>
        request.Operation is "attach" or "detach" or "take-control" or "shutdown" ||
        request.Operation == "command" && request.Payload is { } payload && IsUrgent(payload);

    // Fail closed: new protocol commands are mutations until explicitly classified.
    public static bool IsObservation(JsonElement payload)
    {
        if (payload.ValueKind != JsonValueKind.Object || !payload.TryGetProperty("type", out var type) || type.ValueKind != JsonValueKind.String) return false;
        return type.GetString() switch
        {
            "handshake" or "ping" or "snapshot" or "zoom-snapshot" or "list-capture-devices" or
            "get-output-session" => true,
            "media-core-sync" => payload.TryGetProperty("commands", out var commands) &&
                commands.ValueKind == JsonValueKind.Array && commands.GetArrayLength() == 0,
            _ => false
        };
    }
}
