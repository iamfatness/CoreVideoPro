using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsSourceColumnsTests
{
    [Fact]
    public async Task ColumnsAndIndependentSlotValuesSurviveReload()
    {
        var folder = Path.Combine(Path.GetTempPath(), "cvp-source-columns-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder);
        try
        {
            var path = Path.Combine(folder, "graphics.json");
            var store = new GraphicsDocumentStore(path);
            Assert.Empty((await store.LoadAsync()).Document.SourceColumns);
            await store.SaveAsync(new GraphicsShowDocument
            {
                SourceColumns = [new("location", "Location", new Dictionary<int, string> { [1] = "Boston", [2] = "London" })]
            });
            var column = Assert.Single((await new GraphicsDocumentStore(path).LoadAsync()).Document.SourceColumns);
            Assert.Equal("location", column.Id);
            Assert.Equal("Boston", column.SlotValues[1]);
            Assert.Equal("London", column.SlotValues[2]);
        }
        finally { Directory.Delete(folder, recursive: true); }
    }
}
