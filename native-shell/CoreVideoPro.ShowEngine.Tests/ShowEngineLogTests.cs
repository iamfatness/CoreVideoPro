using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public sealed class ShowEngineLogTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "cvp-show-engine-log-" + Guid.NewGuid().ToString("N"));

    private string Path_ => Path.Combine(_dir, "show-engine.log");

    [Fact]
    public void Append_WritesLinesAndCreatesTheDirectory()
    {
        var log = new ShowEngineLog(Path_);
        log.Append("first");
        log.Append("second");

        var lines = File.ReadAllLines(Path_);
        Assert.Equal(new[] { "first", "second" }, lines);
    }

    [Fact]
    public void Append_OverflowKeepsTheNewestHalf()
    {
        var log = new ShowEngineLog(Path_);
        // Each line is ~1 KB; 400 of them blow well past the 256 KB cap.
        var payload = new string('x', 1000);
        for (var i = 0; i < 400; i++) log.Append(i.ToString("D6") + payload);

        var length = new FileInfo(Path_).Length;
        Assert.InRange(length, 1, ShowEngineLog.MaxBytes);

        var lines = File.ReadAllLines(Path_);
        // the newest line survives; the oldest is gone
        Assert.Equal("000399" + payload, lines[^1]);
        Assert.DoesNotContain("000000" + payload, lines);
        // ~half the cap is retained, so the trim kept the newest half rather than truncating to nothing
        Assert.InRange(length, ShowEngineLog.MaxBytes / 4, ShowEngineLog.MaxBytes);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch { /* best effort */ }
    }
}
