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
    public void RevealWritesItsFeatherAndRoundTrips()
    {
        var scratch = GraphicsTemplateEditor.SetLayerMotion(Seed(), "plate", "in", GraphicsMotionModel.DefaultFor("reveal"));
        Assert.Equal(new GraphicsLayerMotion("reveal", 0, 400, "expo-out", "left", Feather: 32), GraphicsTemplateEditor.LayerMotion(scratch, "plate", "in"));
        var plate = JsonNode.Parse(scratch.Manifest)!["layers"]!.AsArray().First(node => node!["id"]!.GetValue<string>() == "plate")!;
        Assert.Equal(32, plate["animations"]!["in"]!["feather"]!.GetValue<double>());
        Assert.Equal("Reveal from left", GraphicsMotionModel.Bars(scratch, "in")[0].Caption);
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "plate", "in", new("reveal", 0, 400, "quint-out", "up", Feather: 0));
        Assert.Equal(0d, GraphicsTemplateEditor.LayerMotion(scratch, "plate", "in")!.Feather);
        Assert.Equal("Choose an edge from the list.", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerMotion(scratch, "plate", "in", new("wipe", 0, 300, Direction: "left", Feather: 10))).Message);
        Assert.Equal("Choose a feel from the list.", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerMotion(scratch, "plate", "in", new("reveal", 0, 300, "bounce", "left"))).Message);
    }

    [Fact]
    public void StaggerTimesTextMovesAndStaysWithinThreeSeconds()
    {
        var scratch = GraphicsTemplateEditor.SetLayerMotion(Seed(), "name-label", "in", new("rise", 200, 300, Distance: 24));
        Assert.Equal("Only text layers can build line by line or word by word.", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerStagger(scratch, "plate", new("word", 60))).Message);
        Assert.Equal("Give this layer its own motion first.", Assert.Throws<InvalidOperationException>(() =>
            GraphicsTemplateEditor.SetLayerStagger(scratch, "title-label", new("line", 60))).Message);
        scratch = GraphicsTemplateEditor.SetLayerStagger(scratch, "name-label", new("word", 60));
        Assert.Equal(new GraphicsLayerStagger("word", 60), GraphicsTemplateEditor.LayerStagger(scratch, "name-label"));
        // The last of up to 12 words starts 11 * 60 ms after the first.
        Assert.Equal(200 + 300 + 11 * 60, GraphicsMotionModel.Total(scratch, "in"));
        Assert.EndsWith(" · word by word", GraphicsMotionModel.Bars(scratch, "in").Single(bar => bar.LayerId == "name-label").Caption);
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLayerStagger(scratch, "name-label", new("word", 230)));
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLayerMotion(scratch, "name-label", "in", new("rise", 2100, 300, Distance: 24)));
        // Removing the layer's own move removes its stagger with it (the native core requires both).
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "name-label", "in", null);
        Assert.Null(GraphicsTemplateEditor.LayerStagger(scratch, "name-label"));
    }

    [Fact]
    public void LoopsAreValidatedAndCountTowardTheMotionLayerLimit()
    {
        var scratch = GraphicsTemplateEditor.SetLayerLoop(Seed(), "accent", GraphicsMotionModel.DefaultLoop("pulse"));
        Assert.Equal(new GraphicsLayerLoop("pulse", 1200), GraphicsTemplateEditor.LayerLoop(scratch, "accent"));
        scratch = GraphicsTemplateEditor.SetLayerLoop(scratch, "plate", GraphicsMotionModel.DefaultLoop("sweep"));
        Assert.Equal(new GraphicsLayerLoop("sweep", 8000, 1000, "left"), GraphicsTemplateEditor.LayerLoop(scratch, "plate"));
        Assert.EndsWith(" · loops: pulse", GraphicsMotionModel.Bars(scratch, "in").Single(bar => bar.LayerId == "accent").Caption);
        Assert.Equal("A loop repeats every 0.25 to 60 s.", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerLoop(scratch, "accent", new("pulse", 100))).Message);
        Assert.Equal("A sweep crosses within its period.", Assert.Throws<InvalidDataException>(() =>
            GraphicsTemplateEditor.SetLayerLoop(scratch, "accent", new("sweep", 1000, 1500))).Message);
        Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLayerLoop(scratch, "accent", new("spin", 1000)));
        scratch = GraphicsTemplateEditor.SetLayerLoop(scratch, "accent", null);
        Assert.Null(GraphicsTemplateEditor.LayerLoop(scratch, "accent"));
        var full = Seed();
        for (var i = 0; i < 12; ++i) full = GraphicsTemplateEditor.AddLayer(full, "shape").Scratch;
        full = GraphicsTemplateEditor.ApplyMotionPreset(full, "everything-fades");
        full = GraphicsTemplateEditor.AddLayer(full, "shape").Scratch;
        Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.SetLayerLoop(full, "shape-13", GraphicsMotionModel.DefaultLoop("drift")));
    }

    [Fact]
    public void EditorPolishOutputLoadsInTheNativeShapeItWrites()
    {
        var scratch = GraphicsTemplateEditor.SetLayerMotion(Seed(), "plate", "in", GraphicsMotionModel.DefaultFor("reveal"));
        scratch = GraphicsTemplateEditor.SetLayerMotion(scratch, "name-label", "in", new("rise", 200, 300, "quint-out", Distance: 24));
        scratch = GraphicsTemplateEditor.SetLayerStagger(scratch, "name-label", new("word", 60));
        scratch = GraphicsTemplateEditor.SetLayerLoop(scratch, "accent", GraphicsMotionModel.DefaultLoop("drift"));
        var layers = JsonNode.Parse(scratch.Manifest)!["layers"]!.AsArray();
        var name = layers.First(node => node!["id"]!.GetValue<string>() == "name-label")!;
        Assert.Equal("word", name["stagger"]!["by"]!.GetValue<string>());
        Assert.Equal(60, name["stagger"]!["ms"]!.GetValue<double>());
        // Out mirrors In keeps the stagger: both phases share it.
        Assert.NotNull(name["animations"]!["out"]);
        var accent = layers.First(node => node!["id"]!.GetValue<string>() == "accent")!;
        Assert.True(JsonNode.DeepEquals(JsonNode.Parse("""{"type":"drift","periodMs":4000,"direction":"up","distance":6}"""), accent["loop"]));
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
