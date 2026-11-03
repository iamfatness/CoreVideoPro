using System.Text;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class BoundedLogFileTests : IDisposable
{
    private readonly string _directory = Path.Combine(
        Path.GetTempPath(),
        "corevideo-bounded-log-tests",
        Guid.NewGuid().ToString("N"));

    [Fact]
    public void RolloverBoundsFileAndRetainsNewestUtf8Entries()
    {
        var path = Path.Combine(_directory, "media-core.log");
        for (var index = 0; index < 80; index++)
        {
            BoundedLogFile.Append(
                path,
                $"[{index:000}] Elena Kovač — newest diagnostics stay readable{Environment.NewLine}",
                maxBytes: 1024);
        }

        var bytes = File.ReadAllBytes(path);
        var text = new UTF8Encoding(false, true).GetString(bytes);
        Assert.True(bytes.Length <= 1024 + 256, $"bounded log grew to {bytes.Length} bytes");
        Assert.Contains("[log-rollover]", text);
        Assert.Contains("[079] Elena Kovač", text);
        Assert.DoesNotContain("[000] Elena Kovač", text);
    }

    [Fact]
    public void RolloverMarkerNamesTheFirstRetainedEntry()
    {
        var path = Path.Combine(_directory, "media-core.log");
        for (var index = 0; index < 80; index++)
        {
            BoundedLogFile.Append(path, $"[2026-10-27T12:{index:00}:00.000-04:00] [monitor-worker] line {index}{Environment.NewLine}", maxBytes: 1024);
        }

        var text = File.ReadAllText(path, new UTF8Encoding(false, true));
        var marker = text.Split('\n')[0];
        Assert.StartsWith("[log-rollover] older entries removed; retained newest ", marker);
        Assert.Contains("starting at: [2026-10-27T12:", marker);
        // The named entry is the first line after the marker.
        var firstRetained = text.Split('\n')[1];
        Assert.Contains(firstRetained.Substring(0, 30), marker);
    }

    public void Dispose()
    {
        if (Directory.Exists(_directory))
        {
            Directory.Delete(_directory, recursive: true);
        }
    }
}
