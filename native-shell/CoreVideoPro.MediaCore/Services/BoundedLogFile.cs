using System.Collections.Concurrent;
using System.Text;

namespace CoreVideoPro.MediaCore.Services;

/// <summary>
/// Best-effort UTF-8 append with an in-place tail rollover. Studio logs are
/// intentionally bounded: a long soak must not consume the operator's disk,
/// while the newest transitions remain available to support bundles.
/// </summary>
public static class BoundedLogFile
{
    public const long DefaultMaxBytes = 8L * 1024 * 1024;
    private const int MaximumSingleEntryBytes = 1024 * 1024;
    private static readonly ConcurrentDictionary<string, object> Gates =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly UTF8Encoding Utf8 = new(encoderShouldEmitUTF8Identifier: false);

    public static void Append(string path, string text, long maxBytes = DefaultMaxBytes)
    {
        try
        {
            var fullPath = Path.GetFullPath(path);
            var gate = Gates.GetOrAdd(fullPath, static _ => new object());
            lock (gate)
            {
                var directory = Path.GetDirectoryName(fullPath);
                if (!string.IsNullOrWhiteSpace(directory))
                {
                    Directory.CreateDirectory(directory);
                }

                var bytes = Utf8.GetBytes(text);
                if (bytes.Length > MaximumSingleEntryBytes)
                {
                    bytes = bytes[^MaximumSingleEntryBytes..];
                }

                var boundedMax = Math.Max(1024L, maxBytes);
                using var stream = new FileStream(
                    fullPath,
                    FileMode.OpenOrCreate,
                    FileAccess.ReadWrite,
                    FileShare.ReadWrite);
                if (stream.Length + bytes.Length > boundedMax)
                {
                    RetainTail(stream, boundedMax / 2);
                }

                stream.Position = stream.Length;
                stream.Write(bytes);
                stream.Flush(flushToDisk: false);
            }
        }
        catch
        {
            // Logging is diagnostic-only and must never disrupt live media.
        }
    }

    private static void RetainTail(FileStream stream, long requestedBytes)
    {
        var tailLength = (int)Math.Min(Math.Min(requestedBytes, int.MaxValue), stream.Length);
        var tail = new byte[tailLength];
        stream.Position = stream.Length - tailLength;
        var read = 0;
        while (read < tail.Length)
        {
            var count = stream.Read(tail, read, tail.Length - read);
            if (count == 0)
            {
                break;
            }
            read += count;
        }

        var start = 0;
        while (start < read && tail[start] != (byte)'\n')
        {
            start++;
        }
        if (start < read)
        {
            start++;
        }

        var marker = Utf8.GetBytes($"[log-rollover] older entries removed; retained newest {read - start} bytes.{Environment.NewLine}");
        stream.SetLength(0);
        stream.Position = 0;
        stream.Write(marker);
        stream.Write(tail, start, read - start);
    }
}
