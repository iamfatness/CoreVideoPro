using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>The template-first graphic builder's pure decisions (Design mode, #1076 slice 3).</summary>
public sealed class GraphicsBuilderTests
{
    private static NativeGraphicsTemplate Template(string id, string family, int version = 1, params string[] fieldIds) => new() {
        TemplateId = id, TemplateVersion = version, Name = id, Family = family, Runtime = "declarative",
        Fields = fieldIds.Select(f => new NativeGraphicsTemplateField { Id = f, Type = f == "time" ? "string" : "string" }).ToArray() };

    private static NativeGraphicsState State(params NativeGraphicsTemplate[] templates) => new() { Templates = templates };

    private static NativeGraphicsDraft Draft(NativeGraphicsTemplate t, object fields) =>
        new(t.TemplateId, t.TemplateVersion, JsonSerializer.SerializeToElement(fields), new LowerThirdAppearance { AccentColor = "#112233" }, new());

    private static GraphicsPlaylistRow Row(string id, string kind, string hotkey = "", string templateId = "x", int version = 1) =>
        new() { Id = id, Kind = kind, Name = id, Hotkey = hotkey, TemplateId = templateId, TemplateVersion = version };

    [Fact]
    public void FiveKindsCoverTheStarterPlaylist()
    {
        Assert.Equal(["lower-third", "title", "bug", "clock", "sponsor"], GraphicsBuilder.Kinds.Select(k => k.Id));
        Assert.All(GraphicsBuilder.Kinds, k => Assert.Contains(k.RowKind, GraphicsPlaylistRow.Kinds));
    }

    [Fact]
    public void TemplatesForAKindAreTheFamilyNewestVersionOnly()
    {
        var state = State(Template("centered-title", "title"), Template("centered-title", "title", 2), Template("logo-accent", "lower-third"), Template("corner-logo", "bug"));
        var titles = GraphicsBuilder.TemplatesFor(GraphicsBuilder.Kind("title")!, state);
        Assert.Single(titles); Assert.Equal(2, titles[0].TemplateVersion);
        Assert.Empty(GraphicsBuilder.TemplatesFor(GraphicsBuilder.Kind("sponsor")!, state));
        Assert.Empty(GraphicsBuilder.TemplatesFor(GraphicsBuilder.Kind("clock")!, null));
    }

    [Fact]
    public void NextFreeHotkeySkipsTakenKeysExceptTheRowBeingEdited()
    {
        var rows = new[] { Row("a", "title", "F1"), Row("b", "title", "F2"), Row("c", "title", "F4") };
        Assert.Equal("F3", GraphicsBuilder.NextFreeHotkey(rows));
        Assert.Equal("F1", GraphicsBuilder.NextFreeHotkey(rows, exceptRowId: "a"));
        var full = Enumerable.Range(1, 12).Select(n => Row("r" + n, "title", "F" + n)).ToArray();
        Assert.Equal("", GraphicsBuilder.NextFreeHotkey(full));
    }

    [Theory]
    [InlineData("Show title", "show-title")]
    [InlineData("  Guest — Dr. Ana Núñez!  ", "guest-dr-ana-n-ez")]
    [InlineData("2nd half", "nd-half")]
    [InlineData("###", "graphic")]
    public void NewRowIdIsAValidUniqueSlug(string name, string expected)
    {
        var id = GraphicsBuilder.NewRowId(name, []);
        Assert.Equal(expected, id); Assert.True(GraphicsPlaylistRow.ValidId(id));
        Assert.Equal(expected + "-2", GraphicsBuilder.NewRowId(name, [Row(expected, "title")]));
    }

    [Fact]
    public void LowerThirdSavesAsGuestWhenItFollowsSourcesElseAsTitle()
    {
        var t = Template("logo-accent", "lower-third", 1, "name", "title");
        var draft = Draft(t, new { name = "Example guest", title = "Host" });
        var kind = GraphicsBuilder.Kind("lower-third")!;
        var guest = GraphicsBuilder.RowFrom(kind, t, draft, "Guest name", "F1", 8000, followSources: true, rows: []);
        Assert.Equal(GraphicsPlaylistRow.Guest, guest.Kind); Assert.True(guest.FollowSources); Assert.True(guest.IsValid);
        Assert.Equal("Host", guest.Fields["title"].GetString()); Assert.Equal("#112233", guest.Style.AccentColor);
        var fixedText = GraphicsBuilder.RowFrom(kind, t, draft, "Host card", "F2", 0, followSources: false, rows: []);
        Assert.Equal(GraphicsPlaylistRow.Title, fixedText.Kind); Assert.False(fixedText.FollowSources); Assert.True(fixedText.IsValid);
    }

    [Fact]
    public void ClockTemplatesTurnOnTheRowClockAndBadInputsFallBack()
    {
        var t = Template("clock-panel", "clock", 1, "label", "time");
        var row = GraphicsBuilder.RowFrom(GraphicsBuilder.Kind("clock")!, t, Draft(t, new { label = "LIVE" }), "   ", "F99", 1234, false, []);
        Assert.True(row.ShowClock); Assert.Equal("clock-panel", row.Name); Assert.Equal("", row.Hotkey); Assert.Equal(0, row.PlayOutAfterMs);
        Assert.Equal(GraphicsPlaylistRow.LiveBug, row.Kind); Assert.True(row.IsValid);
    }

    [Fact]
    public void EditingKeepsTheRowIdSoShotLayoutsStillFindIt()
    {
        var t = Template("centered-title", "title", 1, "heading");
        var existing = Row("title", GraphicsPlaylistRow.Title, "F2", "centered-title") with { ImagePath = @"C:\x.png" };
        var row = GraphicsBuilder.RowFrom(GraphicsBuilder.Kind("title")!, t, Draft(t, new { heading = "Welcome back" }), "Renamed", "F7", 0, false, [existing], existing);
        Assert.Equal("title", row.Id); Assert.Equal("Renamed", row.Name); Assert.Equal("F7", row.Hotkey); Assert.Equal(@"C:\x.png", row.ImagePath);
        var rows = GraphicsBuilder.Upsert([existing, Row("logo", GraphicsPlaylistRow.Logo)], row);
        Assert.Equal(2, rows.Count); Assert.Equal("Renamed", rows[0].Name);
    }

    [Fact]
    public void NewRowsLandBeforeTheSponsorPlaceholder()
    {
        var placeholder = new GraphicsPlaylistRow { Id = "sponsor", Kind = GraphicsPlaylistRow.Sponsor, Name = "Sponsor card" };
        var rows = GraphicsBuilder.Upsert([Row("guest", GraphicsPlaylistRow.Guest), placeholder], Row("thanks", GraphicsPlaylistRow.Title));
        Assert.Equal(["guest", "thanks", "sponsor"], rows.Select(r => r.Id));
    }

    [Fact]
    public void KindOfARowFollowsItsTemplateFamilyThenItsRowKind()
    {
        var state = State(Template("centered-title", "title"), Template("corner-logo", "bug"));
        Assert.Equal("title", GraphicsBuilder.KindOf(Row("t", GraphicsPlaylistRow.Title, "", "centered-title"), state).Id);
        Assert.Equal("bug", GraphicsBuilder.KindOf(Row("l", GraphicsPlaylistRow.Logo, "", "corner-logo"), state).Id);
        Assert.Equal("lower-third", GraphicsBuilder.KindOf(Row("g", GraphicsPlaylistRow.Guest, "", "compact-solid"), null).Id);
        Assert.Equal("clock", GraphicsBuilder.KindOf(Row("c", GraphicsPlaylistRow.LiveBug, "", "unknown-pkg"), state).Id);
        Assert.Equal("sponsor", GraphicsBuilder.KindOf(new GraphicsPlaylistRow { Id = "s", Kind = GraphicsPlaylistRow.Sponsor, Name = "S" }, state).Id);
    }
}

public sealed class GraphicsBuilderMotionTests
{
    [Fact]
    public void PresetsRoundTripAndStayInsideTheCoreWhitelist()
    {
        foreach (var (id, _) in GraphicsBuilder.MotionPresets)
            foreach (var (_, ms) in GraphicsBuilder.MotionSpeeds) {
                var motion = GraphicsBuilder.MotionFor(id, ms);
                Assert.True(GraphicsPlaylistRow.ValidMotion(motion), id);
                var (back, backMs) = GraphicsBuilder.MotionPresetOf(motion);
                Assert.Equal(id, back);
                if (id.StartsWith("slide")) Assert.Equal(ms, backMs);
            }
        Assert.Null(GraphicsBuilder.MotionFor("default", 300));
        Assert.Equal("none", GraphicsBuilder.MotionFor("cut", 300)!.In.Type);
        var slide = GraphicsBuilder.MotionFor("slide-up", 600)!;
        Assert.Equal(("slide", "up", 600d), (slide.In.Type, slide.In.Direction, slide.In.DurationMs)); Assert.Equal(400d, slide.Out.DurationMs);
    }

    [Fact]
    public void RowValidationRejectsMotionTheCoreWouldRefuse()
    {
        var row = new GraphicsPlaylistRow { Id = "t", Kind = GraphicsPlaylistRow.Title, Name = "T" };
        Assert.True(row.IsValid);
        Assert.True((row with { Motion = GraphicsBuilder.MotionFor("slide-left", 300) }).IsValid);
        Assert.False((row with { Motion = new() { In = new() { Type = "wipe" } } }).IsValid);
        Assert.False((row with { Motion = new() { In = new() { Type = "fade", DurationMs = 5000 } } }).IsValid);
        Assert.False((row with { Motion = new() { Out = new() { Type = "slide", Direction = "diagonal" } } }).IsValid);
    }
}

public sealed class GraphicsBuilderNamesListTests
{
    [Fact]
    public void ParsesLinesCsvHeadersQuotesAndKeepsIdsUnique()
    {
        var text = "Name,Title\r\n\"Lovelace, Ada\", Analyst\nAlan Turing\tMathematician\n\nAlan Turing; Codebreaker\n   \n";
        var list = GraphicsBuilder.ParseNamesList(text);
        Assert.Equal(["Lovelace, Ada", "Alan Turing", "Alan Turing"], list.Select(v => v.Name));
        Assert.Equal(["Analyst", "Mathematician", "Codebreaker"], list.Select(v => v.Title));
        Assert.Equal(["lovelace-ada", "alan-turing", "alan-turing-2"], list.Select(v => v.Id));
        Assert.True(GraphicsPlaylistRow.ValidVariants(list));
        Assert.Empty(GraphicsBuilder.ParseNamesList("name, title"));
        Assert.Empty(GraphicsBuilder.ParseNamesList(""));
    }

    [Fact]
    public void ReSavingKeepsTheIdOfANameAlreadyInTheList()
    {
        var before = GraphicsBuilder.ParseNamesList("Ada, One\nAlan, Two");
        var after = GraphicsBuilder.ParseNamesList("Grace, Zero\nAlan, Two (edited)\nAda, One", before);
        Assert.Equal(before.Single(v => v.Name == "Alan").Id, after.Single(v => v.Name == "Alan").Id);
        Assert.Equal(before.Single(v => v.Name == "Ada").Id, after.Single(v => v.Name == "Ada").Id);
        Assert.Equal("Two (edited)", after.Single(v => v.Name == "Alan").Title);
        Assert.Equal("Grace, Zero" + Environment.NewLine + "Alan, Two (edited)" + Environment.NewLine + "Ada, One", GraphicsBuilder.NamesListText(after));
    }

    [Fact]
    public void ANamesListRowIsATitleKindRowWithVariantsAndNoSourceFollowing()
    {
        var t = new NativeGraphicsTemplate { TemplateId = "logo-accent", TemplateVersion = 1, Name = "Logo accent", Family = "lower-third", Runtime = "declarative" };
        var draft = new NativeGraphicsDraft(t.TemplateId, 1, JsonSerializer.SerializeToElement(new { name = "x", title = "y" }), new LowerThirdAppearance(), new());
        var variants = GraphicsBuilder.ParseNamesList("Ada, Analyst\nAlan, Mathematician");
        var row = GraphicsBuilder.RowFrom(GraphicsBuilder.Kind("lower-third")!, t, draft, "Panelists", "F6", 8000, followSources: true, rows: [], variants: variants);
        Assert.Equal(GraphicsPlaylistRow.Title, row.Kind); Assert.False(row.FollowSources); Assert.True(row.HasVariants); Assert.True(row.IsValid);
        var entries = GraphicsPlayoutService.VariantEntries(row);
        Assert.Equal(["variant:ada", "variant:alan"], entries.Select(e => e.SourceId));
        Assert.Equal(("Ada", "Analyst"), (entries[0].Label, entries[0].Title));
        Assert.True(entries[0].IsVariant);
        // Bare hotkey steps through the list and wraps.
        Assert.Equal("variant:ada", GraphicsPlayoutService.NextVariant(row, [])!.SourceId);
        Assert.Equal("variant:alan", GraphicsPlayoutService.NextVariant(row, ["variant:ada"])!.SourceId);
        Assert.Equal("variant:ada", GraphicsPlayoutService.NextVariant(row, ["variant:alan"])!.SourceId);
        Assert.Null(GraphicsPlayoutService.NextVariant(row with { Variants = [] }, []));
    }

    [Fact]
    public void VariantValidationBoundsTheList()
    {
        var row = new GraphicsPlaylistRow { Id = "p", Kind = GraphicsPlaylistRow.Title, Name = "P" };
        Assert.False((row with { Variants = [new() { Id = "Bad Id", Name = "x" }] }).IsValid);
        Assert.False((row with { Variants = [new() { Id = "a", Name = "x" }, new() { Id = "a", Name = "y" }] }).IsValid);
        Assert.False((row with { Variants = [new() { Id = "a", Name = "" }] }).IsValid);
        Assert.False((row with { Variants = Enumerable.Range(0, 65).Select(i => new GraphicsPlaylistVariant { Id = "v" + i, Name = "n" }).ToArray() }).IsValid);
        Assert.True((row with { Variants = Enumerable.Range(0, 64).Select(i => new GraphicsPlaylistVariant { Id = "v" + i, Name = "n" }).ToArray() }).IsValid);
    }
}

public sealed class GraphicsBuilderPlacementTests
{
    private static NativeGraphicsTemplate Template() => new() { TemplateId = "centered-title", TemplateVersion = 1, Name = "T", Family = "title", Runtime = "declarative",
        DefaultPlacement = new() { Anchor = "upper-center", X = 0, Y = 120, Scale = 1 } };

    [Fact]
    public void ARowKeepsOnlyAPlacementThatDiffersFromTheTemplateDefault()
    {
        var t = Template(); var kind = GraphicsBuilder.Kind("title")!;
        var same = new NativeGraphicsDraft(t.TemplateId, 1, JsonSerializer.SerializeToElement(new { heading = "x" }), new LowerThirdAppearance(), new(), t.DefaultPlacement);
        Assert.Null(GraphicsBuilder.RowFrom(kind, t, same, "A", "", 0, false, []).Placement);
        var moved = same with { Placement = t.DefaultPlacement! with { Anchor = "lower-right", Scale = .8 } };
        var row = GraphicsBuilder.RowFrom(kind, t, moved, "B", "", 0, false, []);
        Assert.Equal(("lower-right", .8), (row.Placement!.Anchor, row.Placement.Scale)); Assert.True(row.IsValid);
        Assert.False((row with { Placement = row.Placement with { Anchor = "nowhere" } }).IsValid);
        Assert.Null(GraphicsBuilder.RowFrom(kind, t, same with { Placement = null }, "C", "", 0, false, []).Placement);
    }
}

public sealed class GraphicsBuilderClockTests
{
    [Fact]
    public void AClockRowCarriesItsSettingsOnlyWhenTheTemplateHasAClock()
    {
        var clockTemplate = new NativeGraphicsTemplate { TemplateId = "clock-panel", TemplateVersion = 1, Name = "Clock", Family = "clock", Runtime = "declarative" };
        var title = new NativeGraphicsTemplate { TemplateId = "centered-title", TemplateVersion = 1, Name = "T", Family = "title", Runtime = "declarative" };
        var draft = new NativeGraphicsDraft("x", 1, JsonSerializer.SerializeToElement(new { label = "LIVE" }), new LowerThirdAppearance(), new());
        var countdown = new NativeGraphicsClockConfiguration { Mode = "countdown", DurationSeconds = 90, Finish = "hide" };
        var row = GraphicsBuilder.RowFrom(GraphicsBuilder.Kind("clock")!, clockTemplate, draft, "Break", "", 0, false, [], clock: countdown);
        Assert.True(row.ShowClock); Assert.Equal(90, row.Clock!.DurationSeconds); Assert.Equal("hide", row.Clock.Finish); Assert.True(row.IsValid);
        Assert.Null(GraphicsBuilder.RowFrom(GraphicsBuilder.Kind("title")!, title, draft, "T", "", 0, false, [], clock: countdown).Clock);
        Assert.False((row with { Clock = countdown with { DurationSeconds = 0 } }).IsValid);
    }
}
