using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsLocalDataTests
{
    [Fact]
    public void KeyedCsvSortsNumbersAndBreaksTiesByStableKey()
    {
        var data = GraphicsLocalData.Parse("id,player,score\nz,\"Smith, Z\",12\na,Ada,12\nb,Bob,9", "csv", "id");
        Assert.Equal(["Ada", "Smith, Z", "Bob"], data.Project(["player", "score"], "score", 12).Select(row => row[0]));
    }

    [Fact]
    public void JsonUsesTheSameTypedProjection()
    {
        var data = GraphicsLocalData.Parse("""
            [{"id":"a","name":"Ada","score":2},{"id":"b","name":"Bob","score":10}]
            """, "json", "id");
        Assert.Equal(["Bob", "Ada"], data.Project(["name"], "score", 12).Select(row => row[0]));
    }

    [Theory]
    [InlineData("id,score\na,2\na,3")]
    [InlineData("id,score,score\na,2,3")]
    [InlineData("id,score\na,2,3")]
    public void InvalidCsvIsRejected(string text) =>
        Assert.Throws<InvalidDataException>(() => GraphicsLocalData.Parse(text, "csv", "id"));

    [Fact]
    public void OversizedInputIsRejectedBeforeParsing() =>
        Assert.Throws<InvalidDataException>(() => GraphicsLocalData.Parse(new string('a', 1024 * 1024 + 1), "csv", null));
}

public sealed class GraphicsCrawlDataTests
{
    [Fact]
    public void SavedCrawlBindingMaterializesFileOrderAndRevision()
    {
        var binding = JsonSerializer.Deserialize<GraphicsDataBinding>("""
            {"sourceId":"crawl","columns":["line"],"limit":64,"revision":3,"crawl":{"speed":160,"gap":25,"direction":"right","mode":"single"}}
            """, new JsonSerializerOptions(JsonSerializerDefaults.Web))!;
        var data = GraphicsLocalData.Parse("line\nFirst\nSecond", "csv", null);
        var manifest = GraphicsDataTable.Manifest("design-crawl", "Crawl", binding, data);
        var restored = GraphicsDataTable.Binding(JsonSerializer.SerializeToElement(manifest));
        Assert.NotNull(restored);
        Assert.Equal(3, restored.Revision);
        var fields = GraphicsDataTable.Fields(data, restored);
        Assert.Equal("[\"First\",\"Second\"]", fields["crawl-items"].GetString());
        Assert.Equal("3", fields["crawl-revision"].GetString());
        Assert.Equal("right", manifest["layers"]![0]!["direction"]!.GetValue<string>());
    }
}
