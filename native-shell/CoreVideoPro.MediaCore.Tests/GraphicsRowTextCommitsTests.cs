using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Re-review m1: text typed into an Operate row box, then Show clicked without Enter, must reach the row
/// before Show reads it.</summary>
public sealed class GraphicsRowTextCommitsTests
{
    [Fact]
    public async Task BeforeAirCommitsTheTypedTextAndWaitsForTheSaveBeforeShowRuns()
    {
        var commits = new GraphicsRowTextCommits(); var log = new System.Collections.Concurrent.ConcurrentQueue<string>(); var save = new TaskCompletionSource();
        commits.Track("guest|location", async () => { log.Enqueue("save started"); await save.Task; log.Enqueue("saved Oslo"); });
        var air = commits.BeforeAirAsync().ContinueWith(_ => log.Enqueue("show"), TaskScheduler.Default);
        Assert.Equal(["save started"], log.ToArray());
        Assert.False(air.IsCompleted); // Show waits for the disk write
        save.SetResult();
        await air;
        Assert.Equal(["save started", "saved Oslo", "show"], log.ToArray());
        Assert.Equal("", commits.Owner);
    }

    [Fact]
    public async Task ACommitFromLostFocusStillRunningIsAwaitedAndCommitsRunInOrder()
    {
        var commits = new GraphicsRowTextCommits(); var log = new System.Collections.Concurrent.ConcurrentQueue<string>(); var first = new TaskCompletionSource();
        // LostFocus (the click on Show took focus) started a save that has not finished when Show runs.
        _ = commits.Commit(async () => { await first.Task; log.Enqueue("location=Oslo"); });
        _ = commits.Commit(() => { log.Enqueue("venue=Hall"); return Task.CompletedTask; });
        var air = commits.BeforeAirAsync().ContinueWith(_ => log.Enqueue("show"), TaskScheduler.Default);
        Assert.Empty(log);
        first.SetResult();
        await air;
        Assert.Equal(["location=Oslo", "venue=Hall", "show"], log.ToArray());
    }

    [Fact]
    public async Task CommittedOrClearedTextIsNotCommittedAgain()
    {
        var commits = new GraphicsRowTextCommits(); var count = 0;
        commits.Track("guest|location", () => { count++; return Task.CompletedTask; });
        await commits.Commit(() => { count++; return Task.CompletedTask; }); // Enter committed it
        await commits.BeforeAirAsync();
        Assert.Equal(1, count);
        commits.Track("guest|location", () => { count++; return Task.CompletedTask; });
        commits.Clear();
        await commits.BeforeAirAsync();
        Assert.Equal(1, count);
        // A failed save does not block the next action.
        _ = commits.Commit(() => Task.FromException(new IOException("disk")));
        await commits.Commit(() => { count++; return Task.CompletedTask; });
        Assert.Equal(2, count);
    }

    [Fact] // final re-review I2: one failed row save must not block every later Show / Hide
    public async Task AFailedRowSaveDoesNotBlockTheNextShow()
    {
        var commits = new GraphicsRowTextCommits();
        _ = commits.Commit(() => Task.FromException(new IOException("disk")));
        await commits.BeforeAirAsync(); // must not throw
        commits.Track("guest|location", () => Task.FromException(new IOException("disk")));
        await commits.BeforeAirAsync(); // a pending commit that fails does not throw into Show either
    }
}
