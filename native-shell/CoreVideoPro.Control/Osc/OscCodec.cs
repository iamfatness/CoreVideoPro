using System.Buffers.Binary;
using System.Text;

namespace CoreVideoPro.Control.Osc;

/// <summary>Minimal, dependency-free OSC 1.0 encoder/decoder for the argument types this control
/// surface uses: int32 (i), float32 (f), string (s), true/false (T/F), blob (b). Also decodes
/// #bundle packets into their component messages. All numeric fields are big-endian, strings and
/// blobs are 4-byte aligned per the OSC spec.</summary>
public static class OscCodec
{
    private static readonly byte[] BundleTag = Encoding.ASCII.GetBytes("#bundle\0");

    public static byte[] EncodeMessage(OscMessage message)
    {
        var buffer = new List<byte>(64);
        WriteString(buffer, message.Address);

        var tags = new StringBuilder(",");
        var payload = new List<byte>(32);
        foreach (var arg in message.Args)
        {
            switch (arg)
            {
                case bool b:
                    tags.Append(b ? 'T' : 'F');
                    break;
                case int i:
                    tags.Append('i');
                    WriteInt32(payload, i);
                    break;
                case long l:
                    tags.Append('i');
                    WriteInt32(payload, checked((int)l));
                    break;
                case float f:
                    tags.Append('f');
                    WriteFloat32(payload, f);
                    break;
                case double d:
                    tags.Append('f');
                    WriteFloat32(payload, (float)d);
                    break;
                case string s:
                    tags.Append('s');
                    WriteString(payload, s);
                    break;
                case byte[] blob:
                    tags.Append('b');
                    WriteBlob(payload, blob);
                    break;
                case null:
                    // OSC has no null; encode as an empty string so the slot is preserved.
                    tags.Append('s');
                    WriteString(payload, string.Empty);
                    break;
                default:
                    throw new NotSupportedException($"Unsupported OSC argument type '{arg.GetType()}'.");
            }
        }

        WriteString(buffer, tags.ToString());
        buffer.AddRange(payload);
        return buffer.ToArray();
    }

    /// <summary>Decode a UDP packet into one or more messages (a plain message → one; a #bundle →
    /// its elements). Returns an empty list on a malformed packet rather than throwing, so a bad
    /// datagram can never crash the listener.</summary>
    public static IReadOnlyList<OscMessage> Decode(ReadOnlySpan<byte> packet)
    {
        try
        {
            if (StartsWithBundle(packet))
            {
                var messages = new List<OscMessage>();
                DecodeBundle(packet, messages);
                return messages;
            }

            var offset = 0;
            var message = DecodeMessage(packet, ref offset);
            return message is null ? System.Array.Empty<OscMessage>() : new[] { message };
        }
        catch
        {
            return System.Array.Empty<OscMessage>();
        }
    }

    private static bool StartsWithBundle(ReadOnlySpan<byte> packet) =>
        packet.Length >= BundleTag.Length && packet[..BundleTag.Length].SequenceEqual(BundleTag);

    private static void DecodeBundle(ReadOnlySpan<byte> packet, List<OscMessage> messages)
    {
        // "#bundle\0" (8) + timetag (8) + [ int32 size + element ]*
        var offset = BundleTag.Length + 8;
        while (offset + 4 <= packet.Length)
        {
            var size = BinaryPrimitives.ReadInt32BigEndian(packet.Slice(offset, 4));
            offset += 4;
            if (size < 0 || offset + size > packet.Length)
            {
                break;
            }

            var element = packet.Slice(offset, size);
            if (StartsWithBundle(element))
            {
                DecodeBundle(element, messages);
            }
            else
            {
                var elemOffset = 0;
                var message = DecodeMessage(element, ref elemOffset);
                if (message is not null)
                {
                    messages.Add(message);
                }
            }

            offset += size;
        }
    }

    private static OscMessage? DecodeMessage(ReadOnlySpan<byte> data, ref int offset)
    {
        var address = ReadString(data, ref offset);
        if (address is null || address.Length == 0 || address[0] != '/')
        {
            return null;
        }

        // Type tag string is optional in the wild; treat its absence as no args.
        if (offset >= data.Length)
        {
            return new OscMessage(address, System.Array.Empty<object?>());
        }

        var tags = ReadString(data, ref offset);
        if (tags is null || tags.Length == 0 || tags[0] != ',')
        {
            return new OscMessage(address, System.Array.Empty<object?>());
        }

        var args = new List<object?>(tags.Length - 1);
        for (var i = 1; i < tags.Length; i++)
        {
            switch (tags[i])
            {
                case 'i':
                    args.Add(ReadInt32(data, ref offset));
                    break;
                case 'f':
                    args.Add(ReadFloat32(data, ref offset));
                    break;
                case 's':
                case 'S':
                    args.Add(ReadString(data, ref offset));
                    break;
                case 'T':
                    args.Add(true);
                    break;
                case 'F':
                    args.Add(false);
                    break;
                case 'b':
                    args.Add(ReadBlob(data, ref offset));
                    break;
                case 'N':
                case 'I':
                    args.Add(null);
                    break;
                default:
                    // Unknown tag with unknown width — stop parsing args defensively.
                    return new OscMessage(address, args);
            }
        }

        return new OscMessage(address, args);
    }

    // ---- primitive writers ---------------------------------------------------------------

    private static void WriteInt32(List<byte> buffer, int value)
    {
        Span<byte> tmp = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(tmp, value);
        buffer.AddRange(tmp.ToArray());
    }

    private static void WriteFloat32(List<byte> buffer, float value)
    {
        Span<byte> tmp = stackalloc byte[4];
        BinaryPrimitives.WriteSingleBigEndian(tmp, value);
        buffer.AddRange(tmp.ToArray());
    }

    private static void WriteString(List<byte> buffer, string value)
    {
        var bytes = Encoding.UTF8.GetBytes(value);
        buffer.AddRange(bytes);
        // Null terminator + pad to a 4-byte boundary (at least one null).
        var pad = 4 - (bytes.Length % 4);
        for (var i = 0; i < pad; i++)
        {
            buffer.Add(0);
        }
    }

    private static void WriteBlob(List<byte> buffer, byte[] blob)
    {
        WriteInt32(buffer, blob.Length);
        buffer.AddRange(blob);
        var pad = (4 - (blob.Length % 4)) % 4;
        for (var i = 0; i < pad; i++)
        {
            buffer.Add(0);
        }
    }

    // ---- primitive readers ---------------------------------------------------------------

    private static int ReadInt32(ReadOnlySpan<byte> data, ref int offset)
    {
        var value = BinaryPrimitives.ReadInt32BigEndian(data.Slice(offset, 4));
        offset += 4;
        return value;
    }

    private static float ReadFloat32(ReadOnlySpan<byte> data, ref int offset)
    {
        var value = BinaryPrimitives.ReadSingleBigEndian(data.Slice(offset, 4));
        offset += 4;
        return value;
    }

    private static string? ReadString(ReadOnlySpan<byte> data, ref int offset)
    {
        var start = offset;
        while (offset < data.Length && data[offset] != 0)
        {
            offset++;
        }

        if (offset >= data.Length)
        {
            return null;  // unterminated
        }

        var value = Encoding.UTF8.GetString(data.Slice(start, offset - start));
        // Advance past the null terminator and align to a 4-byte boundary.
        var length = offset - start;
        var padded = length + (4 - (length % 4));
        offset = start + padded;
        return value;
    }

    private static byte[] ReadBlob(ReadOnlySpan<byte> data, ref int offset)
    {
        var size = ReadInt32(data, ref offset);
        if (size < 0 || offset + size > data.Length)
        {
            throw new FormatException("OSC blob size out of range.");
        }

        var blob = data.Slice(offset, size).ToArray();
        var padded = size + ((4 - (size % 4)) % 4);
        offset += padded;
        return blob;
    }
}
