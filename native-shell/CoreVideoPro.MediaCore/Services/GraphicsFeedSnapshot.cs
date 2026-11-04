using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed class GraphicsFeedSnapshot
{
    public const long MaxSequence = 9_007_199_254_740_991;
    private static readonly UTF8Encoding StrictUtf8 = new(false, true);

    private GraphicsFeedSnapshot(long sequence, string keyColumn, GraphicsLocalData data) =>
        (Sequence, KeyColumn, Data) = (sequence, keyColumn, data);

    public long Sequence { get; }
    internal string KeyColumn { get; }
    public GraphicsLocalData Data { get; }

    public static GraphicsFeedSnapshot Parse(ReadOnlySpan<byte> message, string keyColumn)
    {
        if (message.Length > GraphicsLocalData.ByteLimit)
            throw new InvalidDataException("A graphics snapshot can contain at most 1 MiB.");
        if (string.IsNullOrWhiteSpace(keyColumn))
            throw new InvalidDataException("Choose a stable row key.");

        try
        {
            using var document = JsonDocument.Parse(StrictUtf8.GetString(message), new JsonDocumentOptions { MaxDepth = 5 });
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                root.EnumerateObject().Count() != 3 ||
                !root.TryGetProperty("schemaVersion", out var version) || version.ValueKind != JsonValueKind.Number ||
                !version.TryGetInt32(out var schema) || schema != 1 ||
                !root.TryGetProperty("sequence", out var order) || order.ValueKind != JsonValueKind.Number ||
                !order.TryGetInt64(out var sequence) || sequence is < 1 or > MaxSequence ||
                !root.TryGetProperty("rows", out var rows) || rows.ValueKind != JsonValueKind.Array)
                throw new InvalidDataException("Invalid graphics snapshot envelope.");

            return new(sequence, keyColumn, GraphicsLocalData.Parse(rows.GetRawText(), "json", keyColumn));
        }
        catch (Exception ex) when (ex is DecoderFallbackException or JsonException or InvalidOperationException)
        {
            throw new InvalidDataException("A graphics snapshot needs valid UTF-8 JSON.", ex);
        }
    }
}
