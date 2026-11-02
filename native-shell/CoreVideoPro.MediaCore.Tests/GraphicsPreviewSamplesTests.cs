using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsPreviewSamplesTests
{
    private static NativeGraphicsScratch Seed =>
        NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest("lower-third", "design-test-a1b2c3", 1, "Test")));

    private static NativeGraphicsTemplate TemplateOf(NativeGraphicsScratch scratch) =>
        new() { Runtime = "declarative", TemplateId = "design-test-a1b2c3", TemplateVersion = 1,
            Fields = GraphicsTemplateEditor.TemplateFields(scratch),
            Definition = JsonDocument.Parse(scratch.Manifest).RootElement.Clone() };

    private static NativeGraphicsDraft DraftOf(NativeGraphicsScratch scratch, object fields) =>
        new("design-test-a1b2c3", 1, JsonSerializer.SerializeToElement(fields), new(), new(), null, scratch);

    [Fact]
    public void LongestNameFillsOnlyTheFirstTextVariable()
    {
        var seed = Seed;
        var v = GraphicsPreviewSamples.Apply(DraftOf(seed, new { name = "Ada", title = "Host" }), TemplateOf(seed), "longest-name");
        Assert.Equal(GraphicsPreviewSamples.LongestName, v.GetProperty("name").GetString());
        Assert.Equal("Host", v.GetProperty("title").GetString());
    }

    [Fact]
    public void LongestNameRespectsMaxLength()
    {
        var scratch = GraphicsTemplateEditor.SetFieldOptions(Seed, "name", 10, null, false);
        var v = GraphicsPreviewSamples.Apply(DraftOf(scratch, new { name = "Ada", title = "Host" }), TemplateOf(Seed), "longest-name");
        Assert.Equal("Maximilian", v.GetProperty("name").GetString());
    }

    [Fact]
    public void SamplesCoverVariablesAddedInTheDraft()
    {
        var installed = TemplateOf(Seed);
        var scratch = GraphicsTemplateEditor.AddField(Seed, "location", "text", 40);
        var (withLayer, layerId) = GraphicsTemplateEditor.AddLayer(scratch, "text");
        scratch = GraphicsTemplateEditor.BindLayer(withLayer, layerId, "text", "location", null);
        var v = GraphicsPreviewSamples.Apply(DraftOf(scratch, new { name = "Ada", title = "Host", location = "Oslo" }), installed, "blank");
        Assert.Equal("", v.GetProperty("location").GetString());
        Assert.Equal("", v.GetProperty("title").GetString());
    }
}
