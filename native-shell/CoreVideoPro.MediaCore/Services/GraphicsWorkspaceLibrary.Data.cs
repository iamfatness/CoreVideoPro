using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsDataSource(string Id, string Name, long Revision, DateTimeOffset ReceivedAt, GraphicsLocalData Data)
{
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public long? FeedSequence { get; init; }

    public bool Valid() => (FeedSequence is null or >= 1 and <= GraphicsFeedSnapshot.MaxSequence) && Id is not null && GraphicsPlaylistRow.ValidId(Id) &&
        Name is { Length: > 0 and <= 128 } && Revision > 0 && ReceivedAt != default && Data is not null && Data.Valid();
}

public sealed partial class GraphicsWorkspaceLibrary
{
    public async Task<GraphicsDataSource?> SaveLocalDataAsync(string id, string name, GraphicsLocalData data)
    {
        await LoadAsync();
        if (Document.Feeds.Any(feed => feed.Id == id && feed.Format == "wss"))
        {
            Fail("Use a new dataset ID for local data.");
            return null;
        }
        var previous = Document.DataSources.FirstOrDefault(source => source.Id == id);
        var source = new GraphicsDataSource(id, name, checked((previous?.Revision ?? 0) + 1), DateTimeOffset.UtcNow, data);
        var sources = Document.DataSources.Where(item => item.Id != id).Append(source).ToArray();
        return await Save(Document with { DataSources = sources }, $"Loaded {source.Name} revision {source.Revision}.") ? source : null;
    }
}
