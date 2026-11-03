using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsMappedValuesTests
{
    [Fact]
    public void SlotValuesUseTheMappedColumnAndPinnedVariableType()
    {
        var row = new GraphicsPlaylistRow { FollowSources = true, SourceMappings = new Dictionary<string, string> { ["location"] = "office", ["count"] = "people" } };
        GraphicsSourceColumn[] columns = [new("office", "Office", new Dictionary<int, string> { [2] = "Boston" }),
            new("people", "People", new Dictionary<int, string> { [2] = "999" })];
        NativeGraphicsTemplateField[] fields = [new() { Id = "location", Type = "string", MaxLength = 20 },
            new() { Id = "count", Type = "number", Maximum = 100 }];
        var values = new Dictionary<string, JsonElement> { ["count"] = JsonSerializer.SerializeToElement(4) };
        GraphicsMappedValues.Apply(row, new GraphicsPlaylistEntry { SlotNumber = 2 }, fields, columns, values);
        Assert.Equal("Boston", values["location"].GetString());
        Assert.Equal(4, values["count"].GetInt32());
    }
}
