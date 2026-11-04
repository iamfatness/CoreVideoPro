using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsFeedSnapshotTests : IDisposable
{
    private static readonly GraphicsFeedDefinition Feed = new("results", "Results", "wss",
        "wss://feed.example.org/results", "id", Enabled: true);
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "cvp-push-" + Guid.NewGuid().ToString("N"));
    private string DocumentPath => Path.Combine(_directory, "graphics.json");

    private GraphicsWorkspaceLibrary Open() => new(new GraphicsDocumentStore(DocumentPath),
        new GraphicsWorkspaceSession((_, _, _) => throw new InvalidOperationException(), () => null));

    [Fact]
    public async Task WebSocketDefinitionRoundTripsAsAnEnabledFeed()
    {
        var saved = Assert.IsType<GraphicsFeedDefinition>(await Open().SaveFeedAsync(Feed));
        var reopened = Open();
        await reopened.LoadAsync();
        Assert.Equal(saved, Assert.Single(reopened.Document.Feeds));
        Assert.Empty(reopened.Document.DataSources);
    }

    [Theory]
    [InlineData("""{"schemaVersion":2,"sequence":1,"rows":[{"id":"a"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":0,"rows":[{"id":"a"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":9007199254740992,"rows":[{"id":"a"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1.5,"rows":[{"id":"a"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1,"sequence":2,"rows":[{"id":"a"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1,"rows":[{"id":"a","id":"b"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1,"rows":[{"id":"a"},{"id":"a"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1,"rows":[{"name":"A"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1,"rows":[{"id":"a","score":1},{"id":"b","score":"text"}]}""")]
    [InlineData("""{"schemaVersion":1,"sequence":1,"rows":[]}""")]
    public void InvalidSnapshotsAreRejected(string json) =>
        Assert.Throws<InvalidDataException>(() => GraphicsFeedSnapshot.Parse(System.Text.Encoding.UTF8.GetBytes(json), "id"));

    [Fact]
    public void SnapshotRequiresStrictUtf8AndAByteBound()
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes("""{"schemaVersion":1,"sequence":1,"rows":[{"id":"a"}]}""");
        bytes[Array.IndexOf(bytes, (byte)'a', bytes.Length - 8)] = 0xFF;
        Assert.Throws<InvalidDataException>(() => GraphicsFeedSnapshot.Parse(bytes, "id"));
        Assert.Throws<InvalidDataException>(() => GraphicsFeedSnapshot.Parse(new byte[GraphicsLocalData.ByteLimit + 1], "id"));
        Assert.Equal(GraphicsFeedSnapshot.MaxSequence, Snapshot(GraphicsFeedSnapshot.MaxSequence).Sequence);
    }

    [Fact]
    public async Task WatermarkAndDataCommitTogetherAndRejectReplayAfterReopen()
    {
        var library = Open();
        var feed = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(Feed));
        var received = DateTimeOffset.UnixEpoch.AddDays(1);
        Assert.Equal(GraphicsFeedStage.Updated, await library.StagePushSnapshotAsync(feed, Snapshot(5), received, default));
        Assert.Equal(GraphicsFeedStage.Unchanged, await library.StagePushSnapshotAsync(feed, Snapshot(5), received.AddSeconds(1), default));
        Assert.Equal(GraphicsFeedStage.Rejected, await library.StagePushSnapshotAsync(feed, Snapshot(5, "30"), received, default));
        Assert.Equal(GraphicsFeedStage.WatermarkAdvanced, await library.StagePushSnapshotAsync(feed, Snapshot(9), received.AddSeconds(2), default));
        Assert.Equal(received, Assert.Single(library.Document.DataSources).ReceivedAt);

        var reopened = Open();
        Assert.Equal(GraphicsFeedStage.StaleSequence, await reopened.StagePushSnapshotAsync(feed, Snapshot(8, "11"), received, default));
        Assert.Equal(GraphicsFeedStage.Rejected, await reopened.StagePushSnapshotAsync(feed, Snapshot(11, "\"wrong\""), received, default));
        var retained = Assert.Single(reopened.Document.DataSources);
        Assert.Equal(9L, retained.FeedSequence);
        Assert.Equal(1L, retained.Revision);
        Assert.Equal("25", retained.Data.Rows.Single().Cells[1]);
        Assert.Equal(GraphicsFeedStage.Updated, await reopened.StagePushSnapshotAsync(feed, Snapshot(14, "31"), received.AddSeconds(3), default));
        var next = Assert.Single((await new GraphicsDocumentStore(DocumentPath).LoadAsync()).Document.DataSources);
        Assert.Equal(14L, next.FeedSequence);
        Assert.Equal(2L, next.Revision);
        Assert.Equal("31", next.Data.Rows.Single().Cells[1]);
    }

    [Fact]
    public async Task FailedSaveAndStaleGenerationLeaveTheDurableSnapshotIntact()
    {
        var library = Open();
        var feed = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(Feed));
        var received = DateTimeOffset.UnixEpoch.AddDays(1);
        Assert.Equal(GraphicsFeedStage.Updated, await library.StagePushSnapshotAsync(feed, Snapshot(5), received, default));
        Assert.Null(await library.SaveFeedAsync(feed with { Endpoint = "wss://other.example.org/results" }));
        Assert.Null(await library.SaveLocalDataAsync(feed.Id, feed.Name, Snapshot(5).Data));
        var edited = Assert.IsType<GraphicsFeedDefinition>(await library.SaveFeedAsync(feed with { Name = "New label" }));
        Assert.Equal(GraphicsFeedStage.Rejected, await library.StagePushSnapshotAsync(feed, Snapshot(6, "27"), received, default));
        await File.AppendAllTextAsync(DocumentPath, " ");
        Assert.Equal(GraphicsFeedStage.Rejected, await library.StagePushSnapshotAsync(edited, Snapshot(6, "27"), received, default));

        foreach (var source in new[] { Assert.Single(library.Document.DataSources),
                     Assert.Single((await new GraphicsDocumentStore(DocumentPath).LoadAsync()).Document.DataSources) })
        {
            Assert.Equal(5L, source.FeedSequence);
            Assert.Equal(1L, source.Revision);
            Assert.Equal("25", source.Data.Rows.Single().Cells[1]);
        }
    }

    private static GraphicsFeedSnapshot Snapshot(long sequence, string score = "25") =>
        GraphicsFeedSnapshot.Parse(System.Text.Encoding.UTF8.GetBytes(
            $$"""{"schemaVersion":1,"sequence":{{sequence}},"rows":[{"id":"a","score":{{score}} }]}"""), "id");

    public void Dispose()
    {
        if (!Directory.Exists(_directory)) return;
        foreach (var file in Directory.GetFiles(_directory)) File.Delete(file);
        Directory.Delete(_directory);
    }
}
