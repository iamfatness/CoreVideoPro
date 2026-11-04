namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsWorkspaceLibrary
{
    public async Task<GraphicsFeedDefinition?> SaveFeedAsync(GraphicsFeedDefinition definition)
    {
        await LoadAsync();
        var previous = Document.Feeds.FirstOrDefault(feed => feed.Id == definition.Id);
        var source = Document.DataSources.FirstOrDefault(item => item.Id == definition.Id);
        if (!definition.Valid() || definition.Generation != (previous?.Generation ?? 0) ||
            (previous is null && source is not null))
        { Fail("Check the feed name, format, URL, stable ID and interval. Reopen settings if the feed changed."); return null; }

        if (source is not null && (definition.Format == "wss" || previous?.Format == "wss") &&
            (previous?.Format != definition.Format || previous.Endpoint != definition.Endpoint || previous.KeyColumn != definition.KeyColumn))
        {
            Fail("Use a new feed for a different WebSocket endpoint, format or row key.");
            return null;
        }
        try
        {
            if (definition.Format == "wss") GraphicsFeedTransport.ValidateWebSocketEndpoint(definition);
            else GraphicsFeedTransport.ValidateEndpoint(definition);
        }
        catch (InvalidDataException ex) { Fail(ex.Message); return null; }

        var saved = definition with { Generation = checked(definition.Generation + 1) };
        var feeds = Document.Feeds.Where(feed => feed.Id != saved.Id).Append(saved).ToArray();
        return await Save(Document with { Feeds = feeds }, "Graphics feed settings saved.") ? saved : null;
    }

    public async Task<GraphicsFeedStage> StageFeedAsync(GraphicsFeedDefinition expected, GraphicsLocalData data,
        DateTimeOffset receivedAt, CancellationToken cancellation)
    {
        await LoadAsync();
        cancellation.ThrowIfCancellationRequested();
        if (expected.Format == "wss" || !CanStageFeed(expected, data, receivedAt)) return GraphicsFeedStage.Rejected;

        var previous = Document.DataSources.FirstOrDefault(source => source.Id == expected.Id);
        if (previous is not null)
        {
            if (!previous.Data.Columns.SequenceEqual(data.Columns)) return GraphicsFeedStage.Rejected;
            if (SameFeedRows(previous.Data, data)) return GraphicsFeedStage.Unchanged;
        }

        var source = new GraphicsDataSource(expected.Id, expected.Name, checked((previous?.Revision ?? 0) + 1), receivedAt, data);
        return await SaveFeedData(source, $"Loaded feed revision {source.Revision}.", cancellation)
            ? GraphicsFeedStage.Updated : GraphicsFeedStage.Rejected;
    }

    public async Task<GraphicsFeedStage> StagePushSnapshotAsync(GraphicsFeedDefinition expected, GraphicsFeedSnapshot snapshot,
        DateTimeOffset receivedAt, CancellationToken cancellation)
    {
        await LoadAsync();
        cancellation.ThrowIfCancellationRequested();
        var data = snapshot.Data;
        if (expected.Format != "wss" || expected.KeyColumn != snapshot.KeyColumn || !CanStageFeed(expected, data, receivedAt))
            return GraphicsFeedStage.Rejected;

        var previous = Document.DataSources.FirstOrDefault(source => source.Id == expected.Id);
        var watermark = previous?.FeedSequence ?? 0;
        if (snapshot.Sequence < watermark) return GraphicsFeedStage.StaleSequence;
        if (previous is not null && !previous.Data.Columns.SequenceEqual(data.Columns)) return GraphicsFeedStage.Rejected;

        var unchanged = previous is not null && SameFeedRows(previous.Data, data);
        if (snapshot.Sequence == watermark)
            return unchanged ? GraphicsFeedStage.Unchanged : GraphicsFeedStage.Rejected;

        foreach (var row in Document.Playlist)
        {
            if (row.DataBinding is not { } binding || binding.SourceId != expected.Id) continue;
            try { GraphicsDataTable.Fields(data, binding); }
            catch (InvalidDataException) { return GraphicsFeedStage.Rejected; }
        }

        if (unchanged)
        {
            var acknowledged = previous! with { FeedSequence = snapshot.Sequence };
            return await SaveFeedData(acknowledged, "Feed sequence advanced.", cancellation)
                ? GraphicsFeedStage.WatermarkAdvanced : GraphicsFeedStage.Rejected;
        }

        var source = new GraphicsDataSource(expected.Id, expected.Name, checked((previous?.Revision ?? 0) + 1), receivedAt, data)
        {
            FeedSequence = snapshot.Sequence
        };
        return await SaveFeedData(source, $"Loaded feed revision {source.Revision}.", cancellation)
            ? GraphicsFeedStage.Updated : GraphicsFeedStage.Rejected;
    }

    private bool CanStageFeed(GraphicsFeedDefinition expected, GraphicsLocalData data, DateTimeOffset receivedAt) =>
        expected.Enabled && Document.Feeds.Contains(expected) && data.Valid() && receivedAt != default && data.Rows.Length > 0;

    private Task<bool> SaveFeedData(GraphicsDataSource source, string status, CancellationToken cancellation)
    {
        var sources = Document.DataSources.Where(item => item.Id != source.Id).Append(source).ToArray();
        return Save(Document with { DataSources = sources }, status, cancellation);
    }

    private static bool SameFeedRows(GraphicsLocalData first, GraphicsLocalData second) =>
        first.Rows.Length == second.Rows.Length && first.Rows.Zip(second.Rows)
            .All(pair => pair.First.Key == pair.Second.Key && pair.First.Cells.SequenceEqual(pair.Second.Cells));
}
