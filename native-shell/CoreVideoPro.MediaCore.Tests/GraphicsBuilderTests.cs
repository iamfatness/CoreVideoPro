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
