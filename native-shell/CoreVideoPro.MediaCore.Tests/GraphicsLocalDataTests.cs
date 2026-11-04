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
