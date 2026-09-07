using System.Text;

namespace CoreVideoPro.ShowEngine;

/// <summary>
/// The engine's own log sink: the child's stderr plus supervisor lifecycle lines. Bounded so a
/// crash-loop on a long show cannot fill the operator's disk — at <see cref="MaxBytes"/> the file is
/// rewritten keeping the NEWEST half, because the lines that explain a failure are the last ones
/// written, not the first. Trimming to a line boundary (not a byte offset) is what keeps the survivor
/// readable.
/// </summary>
public sealed class ShowEngineLog
{
    /// <summary>256 KB.</summary>
    public const long MaxBytes = 256 * 1024;

    private static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false);

    private readonly string _path;
    private readonly object _gate = new();

    /// <param name="path">Log file path; defaults to
    /// <c>%LOCALAPPDATA%\CoreVideoPro\show-engine.log</c>. Injectable so tests never touch it.</param>
    public ShowEngineLog(string? path = null)
    {
        _path = path ?? Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CoreVideoPro",
            "show-engine.log");
    }

    public string FilePath => _path;

    /// <summary>Append one line. Never throws — a log that fails must not take the show with it.</summary>
    public void Append(string line)
    {
        lock (_gate)
        {
            try
            {
                var directory = Path.GetDirectoryName(_path);
                if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

                File.AppendAllText(_path, line + Environment.NewLine, Utf8NoBom);

                var info = new FileInfo(_path);
                if (info.Exists && info.Length > MaxBytes) TrimToNewestHalf();
            }
            catch
            {
                // Best effort by design: diagnostics may never be a failure mode.
            }
        }
    }

    /// <summary>Keep the newest <see cref="MaxBytes"/>/2 BYTES (the cap is a byte budget, and a char
    /// count would under-trim any log carrying non-ASCII), snapped FORWARD past the next newline so
    /// the survivor never begins mid-line. Splitting on 0x0A is UTF-8-safe: that byte cannot occur
    /// inside a multi-byte sequence.</summary>
    private void TrimToNewestHalf()
    {
        var bytes = File.ReadAllBytes(_path);
        var start = Math.Max(0, bytes.Length - (int)(MaxBytes / 2));

        var newline = Array.IndexOf(bytes, (byte)'\n', start);
        if (newline >= 0 && newline + 1 < bytes.Length) start = newline + 1;

        using var stream = new FileStream(_path, FileMode.Create, FileAccess.Write, FileShare.None);
        stream.Write(bytes, start, bytes.Length - start);
    }
}
