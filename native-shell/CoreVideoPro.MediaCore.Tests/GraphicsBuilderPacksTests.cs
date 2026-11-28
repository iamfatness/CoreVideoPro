using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>Graphics styles slice 11: the Builder's style packs, live palette thumbnails, operator knobs and variants.</summary>
public sealed class GraphicsBuilderPacksTests
{
    private static readonly JsonSerializerOptions Web = new(JsonSerializerDefaults.Web);

    private static NativeGraphicsTemplate Look(string id, string family, string? pack, string genre = "", double width = 1000, double height = 150, params string[] fields) => new() {
        TemplateId = id, TemplateVersion = 1, Name = id, Family = family, Runtime = "declarative",
        Pack = pack is null ? null : new() { Id = pack, Name = pack switch { "news-signal" => "News: Signal", "sports-kinetic" => "Sports: Kinetic", _ => pack }, Genre = genre },
        Bounds = JsonSerializer.SerializeToElement(new { width, height }),
        DefaultPlacement = new() { Anchor = family == "lower-third" ? "lower-left" : "upper-right", X = 96, Y = -64 },
        Fields = fields.Select(f => new NativeGraphicsTemplateField { Id = f, Type = "string", Default = JsonSerializer.SerializeToElement(f + "-default") }).ToArray() };

    private static GraphicsBuilderKind LowerThird => GraphicsBuilder.Kind("lower-third")!;

    private static NativeGraphicsState Catalog(params NativeGraphicsTemplate[] templates) => new() { Templates = templates, CoreEpoch = "e" };

    private static readonly NativeGraphicsTemplate[] NewsAndSports = [
        Look("sports-kinetic-lower-third", "lower-third", "sports-kinetic", "sports", 1024, 124, "name"),
        Look("news-signal-name-tag", "lower-third", "news-signal", "news", 1080, 150, "name", "title", "tag"),
        Look("news-signal-lower-third", "lower-third", "news-signal", "news", 1080, 96, "name"),
        Look("news-signal-lower-third-2-line", "lower-third", "news-signal", "news", 1080, 150, "name", "title"),
        Look("news-signal-bug", "bug", "news-signal", "news", 300, 72, "channel", "tag"),
        Look("corporate-lower-third", "lower-third", null, "", 1050, 176, "name", "title"),
    ];

    [Fact]
    public void PacksAreListedInLaunchOrderWithTheirLooksForTheKindOnly()
    {
        var packs = GraphicsBuilder.PacksFor(LowerThird, Catalog(NewsAndSports));
        Assert.Equal(["news-signal", "sports-kinetic"], packs.Select(p => p.Id));
        Assert.Equal("News", packs[0].Label);
        Assert.Equal(["news-signal-lower-third", "news-signal-lower-third-2-line", "news-signal-name-tag"], packs[0].Templates.Select(t => t.TemplateId));
        Assert.DoesNotContain(packs.SelectMany(p => p.Templates), t => t.Family == "bug");
        Assert.Equal(["corporate-lower-third"], GraphicsBuilder.ClassicTemplatesFor(LowerThird, Catalog(NewsAndSports)).Select(t => t.TemplateId));
        Assert.Empty(GraphicsBuilder.PacksFor(GraphicsBuilder.Kind("sponsor")!, Catalog(NewsAndSports)));
        Assert.Empty(GraphicsBuilder.PacksFor(LowerThird, null));
    }

    [Fact]
    public void ANewLowerThirdStartsOnTheFirstPacksTwoLineLook()
    {
        Assert.Equal("news-signal-lower-third-2-line", GraphicsBuilder.DefaultLookFor(LowerThird, Catalog(NewsAndSports))!.TemplateId);
        Assert.Equal("corporate-lower-third", GraphicsBuilder.DefaultLookFor(LowerThird, Catalog(NewsAndSports[5]))!.TemplateId);
        Assert.Null(GraphicsBuilder.DefaultLookFor(LowerThird, Catalog()));
        Assert.Equal(GraphicsBuilder.ClassicStyleId, GraphicsBuilder.StyleOf(NewsAndSports[5]));
        Assert.Equal("news-signal", GraphicsBuilder.StyleOf(NewsAndSports[1]));
    }

    [Fact]
    public void AThumbnailIsCurrentOnlyWhenRenderedInTheBuildersPalette()
    {
        var palette = new Dictionary<string, string> { ["brand.primary"] = "#0B1F3AFF", ["brand.accent"] = "#E63946FF" };
        var look = NewsAndSports[2] with { Thumbnail = new() { State = "ready", PaletteRevision = 3 } };
        var state = Catalog(look) with { TemplateThumbnailPalette = new() { Revision = 3, Palette = palette } };
        Assert.True(GraphicsBuilder.ThumbnailCurrent(look, state, palette));
        // Lower-case hex is the same colour.
        Assert.True(GraphicsBuilder.ThumbnailCurrent(look, state, palette.ToDictionary(p => p.Key, p => p.Value.ToLowerInvariant())));
        // Rendered under an older revision: recolouring, never shown as current.
        Assert.False(GraphicsBuilder.ThumbnailCurrent(look with { Thumbnail = look.Thumbnail! with { PaletteRevision = 2 } }, state, palette));
        // The core is still on the old palette.
        Assert.False(GraphicsBuilder.ThumbnailCurrent(look, state, new Dictionary<string, string>(palette) { ["brand.accent"] = "#00FF00FF" }));
        Assert.False(GraphicsBuilder.ThumbnailCurrent(look with { Thumbnail = new() { State = "pending" } }, state, palette));
        Assert.False(GraphicsBuilder.ThumbnailCurrent(look, Catalog(look), palette));
        // Compatibility presets ignore the palette.
        Assert.True(GraphicsBuilder.ThumbnailCurrent(new NativeGraphicsTemplate { TemplateId = "compact-solid", Runtime = "compatibility" }, state, palette));
    }

    [Fact]
    public void ThePackFieldsParseFromTheCoreCatalogRow()
    {
        var state = JsonSerializer.Deserialize<NativeGraphicsState>("""
            {"coreEpoch":"e","templates":[{"templateId":"news-signal-anchor","templateVersion":1,"runtime":"declarative","family":"lower-third",
              "pack":{"id":"news-signal","name":"News: Signal","genre":"news"},"builtIn":true,
              "thumbnail":{"state":"ready","pngBase64":"","paletteRevision":2}}],
             "templatePacks":[{"id":"news-signal","name":"News: Signal","genre":"news","version":"0.1.0","state":"ready","templates":["news-signal-anchor"]},
                              {"id":"broken","state":"invalid","detail":"template 1: raw hex colour","templates":[]}],
             "templateThumbnailPalette":{"revision":2,"palette":{"brand.primary":"#0B1F3AFF"}}}
            """, Web)!;
        var template = Assert.Single(state.Templates);
        Assert.Equal("news", template.Pack!.Genre);
        Assert.True(template.BuiltIn);
        Assert.Equal(2, template.Thumbnail!.PaletteRevision);
        Assert.Equal(2, state.TemplateThumbnailPalette!.Revision);
        Assert.Equal("#0B1F3AFF", state.TemplateThumbnailPalette.Palette!["brand.primary"]);
        Assert.Equal("invalid", state.TemplatePacks![1].State);
        Assert.Contains("raw hex", state.TemplatePacks![1].Detail);
        Assert.False(template.ExtensionData?.ContainsKey("pack") == true);
    }

    [Fact]
    public void OptionalElementsSwitchBetweenSiblingLooksInThePack()
    {
        var news = GraphicsBuilder.PacksFor(LowerThird, Catalog(NewsAndSports))[0];
        var oneLine = news.Templates[0];
        Assert.Equal("news-signal-lower-third-2-line", GraphicsBuilder.Sibling(news, oneLine, secondLine: true, tag: false)!.TemplateId);
        Assert.Equal("news-signal-name-tag", GraphicsBuilder.Sibling(news, oneLine, secondLine: true, tag: true)!.TemplateId);
        Assert.Same(oneLine, GraphicsBuilder.Sibling(news, oneLine, false, false));
        Assert.Null(GraphicsBuilder.Sibling(news, oneLine, false, true)); // no tag-without-second-line look
        var sports = GraphicsBuilder.PacksFor(LowerThird, Catalog(NewsAndSports))[1];
        Assert.Null(GraphicsBuilder.Sibling(sports, sports.Templates[0], true, false));
        // Typed values travel; new fields take their defaults.
        var carried = GraphicsBuilder.CarryFields(JsonSerializer.SerializeToElement(new { name = "Jordan Ellis", title = "Senior Correspondent" }), news.Templates[2]);
        Assert.Equal("Jordan Ellis", carried.GetProperty("name").GetString());
        Assert.Equal("Senior Correspondent", carried.GetProperty("title").GetString());
        Assert.Equal("tag-default", carried.GetProperty("tag").GetString());
        var dropped = GraphicsBuilder.CarryFields(carried, news.Templates[0]);
        Assert.False(dropped.TryGetProperty("title", out _));
    }

    [Fact]
    public void ColourRolesAreTheTokensTheLookDraws()
    {
        var look = Look("news-signal-name-tag", "lower-third", "news-signal") with {
            BrandTokens = ["brand.fontBody", "brand.primary"],
            Layers = JsonSerializer.SerializeToElement(new object[] {
                new { id = "plate", fill = new { token = "brand.primary" } },
                new { id = "tag", fill = new { gradient = new { stops = new object[] { new { color = new { token = "brand.accent" } } } } } },
                new { id = "tag-text", color = new { token = "brand.onAccent" } },
                new { id = "font", font = new { token = "brand.fontBody" } } }) };
        Assert.Equal([("brand.primary", "Plate"), ("brand.accent", "Accent"), ("brand.onAccent", "Tag text")], GraphicsBuilder.ColourRoles(look));
    }

    [Fact]
    public void PositionsSitOnTheLowerThirdBandInsideGraphicsSafe()
    {
        var look = NewsAndSports[3]; // 1080 x 150
        var left = GraphicsBuilder.PlaceInSafeArea(look, "left", 1, snap: true);
        Assert.Equal(("lower-left", 96d, -80d, 1d), (left.Anchor, left.X, left.Y, left.Scale));
        var right = GraphicsBuilder.PlaceInSafeArea(look, "right", 1, true);
        Assert.Equal(("lower-right", -96d, -80d), (right.Anchor, right.X, right.Y));
        var centre = GraphicsBuilder.PlaceInSafeArea(look, "center", 1, true);
        Assert.Equal(("lower-center", 0d, -80d), (centre.Anchor, centre.X, centre.Y));
        foreach (var placement in new[] { left, right, centre }) Assert.True(GraphicsBuilder.InsideSafeArea(placement, 1080, 150));
        // The band: bottom at y 1000, top at 850 for a 150-tall look.
        Assert.Equal("left", GraphicsBuilder.PositionOf(left)); Assert.Equal("center", GraphicsBuilder.PositionOf(centre));
        // A bug keeps its top row and takes the safe edge.
        var bug = GraphicsBuilder.PlaceInSafeArea(NewsAndSports[4], "left", 1, true);
        Assert.Equal(("upper-left", 96d, 54d), (bug.Anchor, bug.X, bug.Y));
    }

    [Fact]
    public void SnappingShrinksAndMovesALookBackInsideGraphicsSafe()
    {
        var wide = Look("sports-kinetic-stat", "lower-third", "sports-kinetic", "sports", 1600, 180, "name");
        var large = GraphicsBuilder.PlaceInSafeArea(wide, "left", 1.15, snap: true);
        Assert.True(large.Scale <= 1728d / 1600 + 1e-9);
        Assert.True(GraphicsBuilder.InsideSafeArea(large, 1600, 180));
        var free = GraphicsBuilder.PlaceInSafeArea(wide, "left", 1.15, snap: false);
        Assert.Equal(1.15, free.Scale);
        Assert.False(GraphicsBuilder.InsideSafeArea(free, 1600, 180));
        // A full-width ticker runs edge to edge: never snapped or shrunk.
        var ticker = Look("news-signal-ticker", "lower-third", "news-signal", "news", 1920, 64, "label");
        Assert.False(GraphicsBuilder.SnapsToSafeArea(ticker));
        Assert.Equal(1d, GraphicsBuilder.PlaceInSafeArea(ticker, "center", 1, snap: true).Scale);
        Assert.True(GraphicsBuilder.SnapsToSafeArea(wide));
        // A drag that left the safe area is pulled back to its edge.
        var dragged = new NativeGraphicsPlacement { Anchor = "lower-left", X = 10, Y = -2, Scale = 1 };
        var clamped = GraphicsBuilder.ClampToSafeArea(dragged, 1000, 150);
        Assert.Equal((96d, -54d), (clamped.X, clamped.Y));
        Assert.True(GraphicsBuilder.InsideSafeArea(clamped, 1000, 150));
        Assert.Equal("l", GraphicsBuilder.SizeOf(new() { Scale = 1.1 }));
        Assert.Equal("m", GraphicsBuilder.SizeOf(null));
    }

    [Fact]
    public void PackDefaultMotionPlaysThePacksOwnLayeredMotionAtTheChosenSpeed()
    {
        var pack = NewsAndSports[3] with { Animations = new() { In = new() { Type = "fade", DurationMs = 200 }, Out = new() { Type = "fade", DurationMs = 140 } } };
        var motion = GraphicsBuilder.EffectiveMotion(pack, null, 1.5, GraphicsPlayoutService.Motion);
        Assert.Equal("own", motion.Layers); Assert.Equal(200, motion.In.DurationMs); Assert.Equal(1.5, motion.Speed);
        Assert.Null(GraphicsBuilder.EffectiveMotion(pack, null, 1, GraphicsPlayoutService.Motion).Speed);
        var slide = GraphicsBuilder.MotionFor("slide-left", 300)!;
        var picked = GraphicsBuilder.EffectiveMotion(pack, slide, .5, GraphicsPlayoutService.Motion);
        Assert.Equal(("whole", "slide", .5), (picked.Layers, picked.In.Type, picked.Speed!.Value));
        // A classic look without row motion keeps the playout default fade.
        Assert.Equal(GraphicsPlayoutService.Motion, GraphicsBuilder.EffectiveMotion(NewsAndSports[5], null, null, GraphicsPlayoutService.Motion));
        // Speed is on the wire only when it is not 1, and only inside the core's range.
        Assert.DoesNotContain("speed", JsonSerializer.Serialize(GraphicsPlayoutService.Motion, Web));
        Assert.Contains("\"speed\":1.5", JsonSerializer.Serialize(motion, Web));
        Assert.True(GraphicsPlaylistRow.ValidSpeed(2)); Assert.False(GraphicsPlaylistRow.ValidSpeed(5)); Assert.False(GraphicsPlaylistRow.ValidSpeed(double.NaN));
    }

    [Fact]
    public void ASavedRowKeepsItsMotionSpeedAndOmitsItAtOne()
    {
        var look = NewsAndSports[3];
        var draft = new NativeGraphicsDraft(look.TemplateId, 1, JsonSerializer.SerializeToElement(new { name = "Jordan", title = "Host" }), new LowerThirdAppearance(), new());
        var row = GraphicsBuilder.RowFrom(LowerThird, look, draft, "Jordan", "F1", 8000, false, [], motionSpeed: 1.5);
        Assert.Equal(1.5, row.MotionSpeed); Assert.Null(row.Motion); Assert.True(row.IsValid);
        Assert.Contains("\"motionSpeed\":1.5", JsonSerializer.Serialize(row, Web));
        var normal = GraphicsBuilder.RowFrom(LowerThird, look, draft, "Jordan", "F1", 8000, false, [], motionSpeed: 1);
        Assert.Null(normal.MotionSpeed);
        Assert.DoesNotContain("motionSpeed", JsonSerializer.Serialize(normal, Web));
        Assert.False((row with { MotionSpeed = 9 }).IsValid);
    }

    [Fact]
    public void AddSpeakerAppendsALineToTheNamesListBoundToTheSameDesign()
    {
        var text = GraphicsBuilder.AddSpeaker("", "Jordan Ellis", "Senior Correspondent", null, ["name", "title"]);
        text = GraphicsBuilder.AddSpeaker(text, "Sam Ortiz", "Analyst", null, ["name", "title"]);
        Assert.Equal(["Jordan Ellis", "Sam Ortiz"], GraphicsBuilder.Speakers(text));
        var variants = GraphicsBuilder.ParseNamesList(text);
        Assert.Equal("Analyst", variants[1].Title);
        Assert.Equal(text, GraphicsBuilder.AddSpeaker(text, "  ", "nobody", null, null));
        // A tag rides in the tag column when the look has one.
        var tagged = GraphicsBuilder.AddSpeaker(text, "Priya Rao", "Producer", "LIVE", ["name", "title", "tag"]);
        var withTag = GraphicsBuilder.ParseNamesList(tagged, null, ["name", "title", "tag"]);
        Assert.Equal(["Jordan Ellis", "Sam Ortiz", "Priya Rao"], withTag.Select(v => v.Name));
        Assert.Equal("LIVE", withTag[2].Cells["tag"]);
        Assert.Equal(3, withTag.Select(v => v.Id).Distinct().Count());
    }
}
