using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsDataPersistenceTests
{
    [Fact]
    public async Task OlderDocumentLoadsAndNewRevisionSurvivesReopen()
    {
        var path = Path.Combine(Path.GetTempPath(), $"cvp-data-{Guid.NewGuid():N}.json");
        try
        {
            await File.WriteAllTextAsync(path, """{"schemaVersion":3,"showId":"test"}""");
            var store = new GraphicsDocumentStore(path);
            var document = (await store.LoadAsync()).Document;
            Assert.Empty(document.DataSources);
            var data = GraphicsLocalData.Parse("id,score\na,12", "csv", "id");
            await store.SaveAsync(document with { DataSources = [new("scores", "Scores", 1, DateTimeOffset.UtcNow, data)] });
            var reopened = (await new GraphicsDocumentStore(path).LoadAsync()).Document;
            var source = Assert.Single(reopened.DataSources);
            Assert.Equal(1, source.Revision);
            Assert.Equal("12", Assert.Single(source.Data.Rows).Cells[1]);
        }
        finally { File.Delete(path); File.Delete(path + ".bak"); }
    }
}
