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
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    public static async Task<T> ReadAsync<T>(Stream stream, CancellationToken cancellationToken)
    {
        var header = new byte[4];
        await stream.ReadExactlyAsync(header, cancellationToken).ConfigureAwait(false);
        var length = BinaryPrimitives.ReadInt32LittleEndian(header);
        if (length <= 0 || length > MaxBytes) throw new IOException("Engine host message exceeds its byte budget.");
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

    // Fail closed: new protocol commands are mutations until explicitly classified.
    public static bool IsObservation(JsonElement payload)
    {
        if (!payload.TryGetProperty("type", out var type)) return false;
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
