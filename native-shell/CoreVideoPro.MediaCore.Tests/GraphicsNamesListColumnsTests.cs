using Xunit;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsNamesListColumnsTests
{
    [Fact]
    public void RecognizedExtraColumnsRoundTripWithStableVariantIds()
    {
        var existing = GraphicsBuilder.ParseNamesList("Ada, Analyst");
        var variants = GraphicsBuilder.ParseNamesList(
            "name,title,location,team\nAda,Analyst,\"Boston, MA\",\"\"\nAlan,Engineer", existing, ["location", "team"]);

        Assert.Equal(existing[0].Id, variants[0].Id);
        Assert.Equal("Boston, MA", variants[0].Cells["location"]);
        Assert.Equal("", variants[0].Cells["team"]);
        Assert.Empty(variants[1].Cells);

        var roundTrip = GraphicsBuilder.ParseNamesList(GraphicsBuilder.NamesListText(variants), variants, ["location", "team"]);
        Assert.Equal("Boston, MA", roundTrip[0].Cells["location"]);
        Assert.Equal("", roundTrip[0].Cells["team"]);
        Assert.Empty(roundTrip[1].Cells);
    }
}

