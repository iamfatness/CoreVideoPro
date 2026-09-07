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

    private void TrimToNewestHalf()
    {
        var text = File.ReadAllText(_path, Utf8NoBom);
        var keepBytes = MaxBytes / 2;

        // Walk back from the end until the retained tail is under the budget, then snap forward to the
        // next line break so the file never starts mid-line.
        var startChar = Math.Max(0, text.Length - (int)keepBytes);
        var newline = text.IndexOf('\n', startChar);
        if (newline >= 0 && newline + 1 < text.Length) startChar = newline + 1;

        File.WriteAllText(_path, text[startChar..], Utf8NoBom);
    }
}
