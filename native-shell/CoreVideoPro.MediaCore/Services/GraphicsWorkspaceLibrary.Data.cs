using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsDataSource(string Id, string Name, long Revision, DateTimeOffset ReceivedAt, GraphicsLocalData Data)
{
    public bool Valid() => Id is not null && GraphicsPlaylistRow.ValidId(Id) &&
        Name is { Length: > 0 and <= 128 } && Revision > 0 && ReceivedAt != default && Data is not null && Data.Valid();
}

public sealed partial class GraphicsWorkspaceLibrary
{
    public async Task<GraphicsDataSource?> SaveLocalDataAsync(string id, string name, GraphicsLocalData data)
    {
        await LoadAsync();
        var previous = Document.DataSources.FirstOrDefault(source => source.Id == id);
        var source = new GraphicsDataSource(id, name, checked((previous?.Revision ?? 0) + 1), DateTimeOffset.UtcNow, data);
        var sources = Document.DataSources.Where(item => item.Id != id).Append(source).ToArray();
        return await Save(Document with { DataSources = sources }, $"Loaded {source.Name} revision {source.Revision}.") ? source : null;
    }
}
