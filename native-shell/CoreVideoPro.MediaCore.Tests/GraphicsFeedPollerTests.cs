using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsFeedPollerTests
{
    private sealed class Clock : TimeProvider
    {
        public DateTimeOffset Now = new(2026, 11, 4, 12, 0, 0, TimeSpan.Zero);
        public override DateTimeOffset GetUtcNow() => Now;
        public void Advance(int seconds) => Now = Now.AddSeconds(seconds);
    }

    private static readonly GraphicsFeedDefinition Feed = new("feed", "Fixture", "json",
        "https://example.com/feed", "id");
    private static readonly GraphicsLocalData Data = GraphicsLocalData.Parse("""[{"id":"a","value":"First"}]""", "json", "id");

    [Fact]
    public async Task DisabledStartupAndRefreshNowRespectMinimumInterval()
    {
        var clock = new Clock();
        var fetches = 0;
        using var poller = new GraphicsFeedPoller((_, _) => { fetches++; return Task.FromResult(Data); },
            (_, _, _) => Task.FromResult(GraphicsFeedStage.Updated), clock);
        poller.Configure(Feed);
        await poller.TickAsync();
        Assert.Equal(0, fetches);
        poller.Configure(Feed with { Enabled = true });
        await poller.TickAsync();
        Assert.Equal(1, fetches);
        poller.RefreshNow();
        clock.Advance(4);
        await poller.TickAsync();
        Assert.Equal(1, fetches);
        clock.Advance(1);
        await poller.TickAsync();
        Assert.Equal(2, fetches);
    }

    [Fact]
    public async Task ReplacementCancelsOldFetchAndFencesItsLateResult()
    {
        var clock = new Clock();
        var reply = new TaskCompletionSource<GraphicsLocalData>();
        var fetched = new List<string>();
        var staged = new List<string>();
        CancellationToken oldToken = default;
        using var poller = new GraphicsFeedPoller((feed, token) =>
        {
            fetched.Add(feed.Endpoint);
            if (fetched.Count != 1) return Task.FromResult(Data);
            oldToken = token;
            return reply.Task;
        }, (feed, _, _) => { staged.Add(feed.Endpoint); return Task.FromResult(GraphicsFeedStage.Updated); }, clock);
        poller.Configure(Feed with { Enabled = true });
        var first = poller.TickAsync();
        await poller.TickAsync();
        Assert.Single(fetched);
        var replacement = Feed with { Enabled = true, Endpoint = "https://example.com/new" };
        poller.Configure(replacement);
        Assert.True(oldToken.IsCancellationRequested);
        clock.Advance(5);
        await poller.TickAsync();
        Assert.Single(fetched);
        reply.SetResult(Data);
        await first;
        await poller.TickAsync();
        Assert.Equal(new[] { replacement.Endpoint }, staged);
    }

    [Fact]
    public async Task FailureBacksOffAndIdenticalRecoveryAdvancesFreshness()
    {
        var clock = new Clock();
        var stageCount = 0;
        var online = true;
        using var poller = new GraphicsFeedPoller((_, _) => online ? Task.FromResult(Data) : Task.FromException<GraphicsLocalData>(
            new HttpRequestException("Sensitive provider error")), (_, _, _) =>
            Task.FromResult(++stageCount == 1 ? GraphicsFeedStage.Updated : GraphicsFeedStage.Unchanged), clock);
        poller.Configure(Feed with { Enabled = true });
        await poller.TickAsync();
        var accepted = poller.Status.LastSuccess;
        Assert.Equal(clock.Now, accepted);
        online = false;
        clock.Advance(15);
        await poller.TickAsync();
        Assert.Equal("unavailable", poller.Status.Failure);
        Assert.Equal(accepted, poller.Status.LastSuccess);
        clock.Advance(15);
        Assert.Equal("stale", poller.Status.State);
        await poller.TickAsync();
        Assert.Equal(1, stageCount);
        for (var i = 0; i < 8; i++)
        {
            clock.Now = poller.Status.NextCheck!.Value;
            await poller.TickAsync();
            Assert.InRange((poller.Status.NextCheck!.Value - clock.Now).TotalSeconds, 30, 300);
        }
        online = true;
        clock.Now = poller.Status.NextCheck!.Value;
        await poller.TickAsync();
        Assert.Equal("fresh", poller.Status.State);
        Assert.Equal(clock.Now, poller.Status.LastSuccess);
        Assert.Equal(2, stageCount);
    }

    [Fact]
    public async Task SavedFeedRetainsLastGoodRevisionAcrossReconfigureAndReopen()
    {
        var path = Path.Combine(Path.GetTempPath(), "cvp-feed-" + Guid.NewGuid().ToString("N"), "graphics.json");
        var session = new GraphicsWorkspaceSession((_, _, _) => throw new InvalidOperationException(), () => null);
        var store = new GraphicsDocumentStore(path);
        var library = new GraphicsWorkspaceLibrary(store, session);
        try
        {
            await library.LoadAsync();
            Assert.Empty(library.Document.Feeds);
            var saved = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(Feed with { Enabled = true }));
            var now = DateTimeOffset.UtcNow;
            Assert.Equal(GraphicsFeedStage.Updated, await library.StageFeedAsync(saved, Data, now, default));
            Assert.Equal(GraphicsFeedStage.Unchanged, await library.StageFeedAsync(saved, Data, now.AddSeconds(1), default));
            var edited = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(saved with { Enabled = false }));
            Assert.Equal(GraphicsFeedStage.Rejected, await library.StageFeedAsync(saved, Data, now, default));
            var reopened = await new GraphicsDocumentStore(path).LoadAsync();
            Assert.Equal(edited, Assert.Single(reopened.Document.Feeds));
            Assert.Equal(1, Assert.Single(reopened.Document.DataSources).Revision);
        }
        finally
        {
            foreach (var file in Directory.Exists(Path.GetDirectoryName(path)) ? Directory.GetFiles(Path.GetDirectoryName(path)!) : [])
                File.Delete(file);
            if (Directory.Exists(Path.GetDirectoryName(path))) Directory.Delete(Path.GetDirectoryName(path)!);
        }
    }
}