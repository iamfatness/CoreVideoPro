namespace CoreVideoPro.MediaCore.Services;

public sealed partial class GraphicsWorkspaceLibrary
{
    public async Task<GraphicsFeedDefinition?> SaveFeedAsync(GraphicsFeedDefinition definition)
    {
        await LoadAsync();
        var previous = Document.Feeds.FirstOrDefault(feed => feed.Id == definition.Id);
        if (!definition.Valid() || definition.Generation != (previous?.Generation ?? 0) ||
            (previous is null && Document.DataSources.Any(source => source.Id == definition.Id)))
            return null;
        try { GraphicsFeedTransport.ValidateEndpoint(definition); }
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
        if (!expected.Enabled || !Document.Feeds.Contains(expected) || !data.Valid() || receivedAt == default ||
            data.Rows.Length == 0) return GraphicsFeedStage.Rejected;

        var previous = Document.DataSources.FirstOrDefault(source => source.Id == expected.Id);
        if (previous is not null)
        {
            if (!previous.Data.Columns.SequenceEqual(data.Columns)) return GraphicsFeedStage.Rejected;
            if (previous.Data.Rows.Length == data.Rows.Length && previous.Data.Rows.Zip(data.Rows)
                .All(pair => pair.First.Key == pair.Second.Key && pair.First.Cells.SequenceEqual(pair.Second.Cells)))
                return GraphicsFeedStage.Unchanged;
        }
        var source = new GraphicsDataSource(expected.Id, expected.Name, checked((previous?.Revision ?? 0) + 1), receivedAt, data);
        var sources = Document.DataSources.Where(item => item.Id != expected.Id).Append(source).ToArray();
        return await Save(Document with { DataSources = sources }, $"Loaded feed revision {source.Revision}.")
            ? GraphicsFeedStage.Updated : GraphicsFeedStage.Rejected;
    }
}
