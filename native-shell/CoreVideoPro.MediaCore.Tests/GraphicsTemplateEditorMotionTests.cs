using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsTemplateEditorMotionTests
{
    private static NativeGraphicsScratch Seed(string id = "design-test-a1b2c3") =>
        NativeGraphicsScratch.From(JsonSerializer.SerializeToElement(GraphicsDesignSeeds.Manifest("lower-third", id, 1, "Motion fixture")));

    [Fact]
    public void SettingInRecomputesEveryMirroredOut()
    {
        var scratch = GraphicsTemplateEditor.SetLayerMotion(Seed(), "plate", "in", new("wipe", 0, 350, Direction: "left"));
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "name-label", "in", new("rise", 200, 300, Distance: 24));
        Assert.Equal(new GraphicsLayerMotion("wipe", 150, 350, "ease-in", "left"), GraphicsTemplateEditor.LayerMotion(scratch, "plate", "out"));
        Assert.Equal(new GraphicsLayerMotion("rise", 0, 300, "ease-in", Distance: 24), GraphicsTemplateEditor.LayerMotion(scratch, "name-label", "out"));
        Assert.Equal(500, GraphicsMotionModel.Total(scratch, "in"));
        Assert.Equal(500, GraphicsMotionModel.Total(scratch, "out"));
        Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.SetLayerMotion(scratch, "plate", "out", null));
        scratch = GraphicsTemplateEditor.SetOutMirrorsIn(scratch, false);
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "plate", "out", null);
        Assert.Null(GraphicsTemplateEditor.LayerMotion(scratch, "plate", "out"));
    }

    [Fact]
    public void PresetsAndFollowersUsePaintOrderAndTheSpecifiedTiming()
    {
        var scratch = GraphicsTemplateEditor.SetLayerMotion(Seed(), "plate", "in", new("wipe", 0, 350, Direction: "left"));
        var bars = GraphicsMotionModel.Bars(scratch, "in");
        Assert.Equal(new[] { "plate", "accent", "name-label", "title-label" }, bars.Select(bar => bar.LayerId));
        Assert.Equal("Wipe from left", bars[0].Caption);
        Assert.Equal("With the graphic · Fade", bars[1].Caption);
        Assert.True(bars[1].Follows);
        var preset = GraphicsTemplateEditor.ApplyMotionPreset(scratch, "bar-first");
        Assert.Equal(new[] { 0d, 0d, 200d, 300d }, GraphicsMotionModel.Bars(preset, "in").Select(bar => bar.StartMs));
        Assert.Equal(250, GraphicsTemplateEditor.GraphicMotion(preset).Out.DurationMs);
        var build = GraphicsTemplateEditor.ApplyMotionPreset(scratch, "build-left-to-right");
        Assert.Equal(new[] { 0d, 120d, 240d, 360d }, GraphicsMotionModel.Bars(build, "in").Select(bar => bar.StartMs));
        var fade = GraphicsTemplateEditor.ApplyMotionPreset(scratch, "everything-fades");
        Assert.All(GraphicsMotionModel.Bars(fade, "in"), bar => Assert.Equal(("fade", 0d, 250d), (bar.Type, bar.StartMs, bar.DurationMs)));
    }

    [Fact]
    public void MotionLimitsAndDragGrid()
    {
        Assert.Equal("Motion must finish within 3.0 s (starts at + takes).", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerMotion(Seed(), "plate", "in", new("fade", 2800, 300))).Message);
        Assert.Equal("Takes must be at least 0.05 s.", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerMotion(Seed(), "plate", "in", new("fade", 0, 40))).Message);
        Assert.Equal((200d, 300d), GraphicsMotionModel.Drag(180, 300, "body", 22, false));
        Assert.Equal((210d, 290d), GraphicsMotionModel.Drag(200, 300, "start", 12, true));
        Assert.Equal((2700d, 300d), GraphicsMotionModel.Drag(2600, 300, "body", 400, false));
        Assert.Equal((0d, 50d), GraphicsMotionModel.Drag(0, 300, "end", -400, false));
        Assert.Equal(.2, GraphicsMotionModel.ParseSeconds("0.20 s"));
        Assert.Throws<InvalidDataException>(() => GraphicsMotionModel.ParseSeconds("soon"));
    }

    [Fact]
    public void EditorMotionOutputMatchesTheNativeFixture()
    {
        var scratch = GraphicsTemplateEditor.ApplyMotionPreset(Seed("design-fixture-motion"), "bar-first");
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "accent", "in", new("grow", 0, 300, Direction: "down"));
        scratch = GraphicsTemplateEditor.AddLayer(scratch, "shape").Scratch;
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "shape-1", "in", new("pop", 400, 200, "linear"));
        scratch = GraphicsTemplateEditor.AddLayer(scratch, "shape").Scratch;
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "shape-2", "in", new("cut", 450, 0));
        scratch = GraphicsTemplateEditor.AddLayer(scratch, "text").Scratch;
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "text-1", "in", new("fade", 100, 250));
        scratch = GraphicsTemplateEditor.Group(scratch, "title-label").Scratch;
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "group-1", "in", new("slide", 0, 400, "ease-in-out", "left", FromEdge: true));

        var root = new DirectoryInfo(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, "native", "CMakeLists.txt"))) root = root.Parent;
        Assert.NotNull(root);
        var fixture = Path.Combine(root.FullName, "native", "tests", "fixtures", "graphics-editor", "designer-motion.json");
        if (Environment.GetEnvironmentVariable("UPDATE_GRAPHICS_FIXTURES") == "1")
            File.WriteAllText(fixture, JsonNode.Parse(scratch.Manifest)!.ToJsonString(new() { WriteIndented = true }) + Environment.NewLine);
        Assert.True(JsonNode.DeepEquals(JsonNode.Parse(File.ReadAllText(fixture)), JsonNode.Parse(scratch.Manifest)));
    }

    [Fact]
    public void PresetsClearNestedMotionAndUngroupDropsGroupMotion()
    {
        var scratch = GraphicsTemplateEditor.Group(Seed(), "title-label").Scratch;
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "title-label", "in", new("fade", 100, 250));
        scratch = GraphicsTemplateEditor.ApplyMotionPreset(scratch, "bar-first");
        Assert.Null(GraphicsTemplateEditor.LayerMotion(scratch, "title-label", "in"));
        Assert.NotNull(GraphicsTemplateEditor.LayerMotion(scratch, "group-1", "in"));
        scratch = GraphicsTemplateEditor.Ungroup(scratch, "group-1");
        Assert.Null(GraphicsTemplateEditor.LayerMotion(scratch, "title-label", "in"));
        Assert.Null(GraphicsTemplateEditor.LayerMotion(scratch, "group-1", "in"));
    }

    [Fact]
    public void EditingAndDuplicatingRespectTheSixteenLayerLimit()
    {
        var scratch = Seed();
        for (var i = 0; i < 12; ++i) scratch = GraphicsTemplateEditor.AddLayer(scratch, "shape").Scratch;
        scratch = GraphicsTemplateEditor.ApplyMotionPreset(scratch, "everything-fades");
        Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.Duplicate(scratch, "plate", out _));
        scratch = GraphicsTemplateEditor.AddLayer(scratch, "shape").Scratch;
        Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.SetLayerMotion(scratch, "shape-13", "in", new("fade", 0, 100)));
    }
}
