using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsTemplateEditorContentTests
{
    private static NativeGraphicsScratch Seed() => NativeGraphicsScratch.From(
        JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest("lower-third", "design-test-a1b2c3", 1, "Test")));

    [Fact]
    public void NewShapesAndContentControlsWriteNativeProperties()
    {
        var (scratch, ellipse) = GraphicsTemplateEditor.AddLayer(Seed(), "ellipse");
        var (withLine, line) = GraphicsTemplateEditor.AddLayer(scratch, "line");
        scratch = GraphicsTemplateEditor.SetStroke(withLine, ellipse, "#FF0000", 3);
        scratch = GraphicsTemplateEditor.SetStroke(scratch, line, "#FFFFFF", 2, "up");
        scratch = GraphicsTemplateEditor.SetAutoWidth(scratch, "plate", true, 100, 400);
        scratch = GraphicsTemplateEditor.SetTextEffects(scratch, "name-label", 18, "#000000", 2, "#00000080", 2, 4, 5);
        Assert.Equal("Ellipse", GraphicsDesignerModel.Rows(scratch).Single(row => row.Id == ellipse).Kind);
        Assert.Equal("up", GraphicsTemplateEditor.Layer(scratch, line).GetProperty("diagonal").GetString());
        Assert.Equal("auto", GraphicsTemplateEditor.Layer(scratch, "plate").GetProperty("width").GetString());
        Assert.Equal(5, GraphicsTemplateEditor.Layer(scratch, "name-label").GetProperty("shadow").GetProperty("blur").GetInt32());
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetAutoWidth(scratch, "plate", true, 500, 100));
    }
}
