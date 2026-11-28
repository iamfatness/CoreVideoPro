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

    [Fact]
    public void GlassWritesTheNativeBackdropOnAPlateAndRemovesIt()
    {
        var scratch = GraphicsTemplateEditor.SetBackdrop(Seed(), "plate", true, 32, "#0A0F1866");
        var backdrop = GraphicsTemplateEditor.Layer(scratch, "plate").GetProperty("backdrop");
        Assert.Equal(32, backdrop.GetProperty("blur").GetDouble());
        Assert.Equal("#0A0F1866", backdrop.GetProperty("tint").GetString());
        scratch = GraphicsTemplateEditor.SetBackdrop(scratch, "plate", false, 32, "#0A0F1866");
        Assert.False(GraphicsTemplateEditor.Layer(scratch, "plate").TryGetProperty("backdrop", out _));
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetBackdrop(Seed(), "plate", true, 0, "#000000"));
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetBackdrop(Seed(), "plate", true, 65, "#000000"));
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetBackdrop(Seed(), "plate", true, 16, "dark"));
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetBackdrop(Seed(), "name-label", true, 16, "#000000"));
    }
}
