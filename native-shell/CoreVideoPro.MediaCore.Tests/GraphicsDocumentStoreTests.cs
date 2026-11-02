using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsDocumentStoreTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "corevideo-graphics-document-" + Guid.NewGuid().ToString("N"));
    private string DocumentPath => Path.Combine(_directory, "graphics-show.json");
    private static GraphicsShowDocument Example => new() {
        Cues = [new() { Id = "cue-one", Name = "Interview", Draft = new("compact-solid", 1, JsonSerializer.SerializeToElement(new { name = "Ada", title = "" }), new(), new()) }],
        StylePresets = new Dictionary<string, LowerThirdAppearance> { ["Evening"] = new() { NameSize = 64 } },
        SourceOverrides = JsonSerializer.SerializeToElement(new { source = new { title = "", overrideName = true } }) };
    [Fact]
    public async Task RoundTripPreservesExplicitBlankStylesAndUnknownMetadataWithoutRuntimeArming()
    {
        var store = new GraphicsDocumentStore(DocumentPath);
        await store.SaveAsync(Example with { Metadata = new() { ["operatorNote"] = JsonSerializer.SerializeToElement("Keep me") } });
        var result = await new GraphicsDocumentStore(DocumentPath).LoadAsync();
        Assert.False(result.ReadOnly); Assert.Equal("", result.Document.Cues[0].Draft!.Fields.GetProperty("title").GetString());
        Assert.Equal(64, result.Document.StylePresets["Evening"].NameSize);
        Assert.Equal("Keep me", result.Document.Metadata!["operatorNote"].GetString());
        Assert.DoesNotContain("visibility", await File.ReadAllTextAsync(DocumentPath));
        Assert.DoesNotContain("armed", await File.ReadAllTextAsync(DocumentPath));
    }
    [Fact]
    public async Task SchemaOneMigratesInMemoryAndPrivateScratchRoundTripsWithoutAiringState()
    {
        Directory.CreateDirectory(_directory);var old=JsonSerializer.Serialize(Example with {SchemaVersion=1},new JsonSerializerOptions(JsonSerializerDefaults.Web));await File.WriteAllTextAsync(DocumentPath,old);
        var store=new GraphicsDocumentStore(DocumentPath);var loaded=await store.LoadAsync();Assert.Equal(3,loaded.Document.SchemaVersion);Assert.Equal(old,await File.ReadAllTextAsync(DocumentPath));
        var definition=JsonSerializer.SerializeToElement(new {templateId="card",templateVersion=1,layers=new[] {new {id="plate",type="rect",x=0,y=0,width=640,height=200,fill="#123456"}}});
        var scratch=NativeGraphicsScratch.From(definition);var cue=Example.Cues[0] with {Draft=new("card",1,JsonSerializer.SerializeToElement(new {name="Ada"}),new(),new(),Scratch:scratch)};
        await store.SaveAsync(loaded.Document with {Cues=[cue]});var restored=await new GraphicsDocumentStore(DocumentPath).LoadAsync();
        Assert.False(restored.ReadOnly);Assert.Equal(scratch,restored.Document.Cues[0].Draft!.Scratch);Assert.DoesNotContain("visibility",await File.ReadAllTextAsync(DocumentPath));
        Assert.Equal(old,await File.ReadAllTextAsync(DocumentPath+".bak"));
    }
    [Fact]
    public async Task CorruptedPrimaryRecoversBackupAndPreservesBothBeforeSavingAgain()
    {
        var store = new GraphicsDocumentStore(DocumentPath);
        await store.SaveAsync(Example); await store.SaveAsync(Example with { ShowId = "second" });
        await File.WriteAllTextAsync(DocumentPath, "{interrupted");
        var recovery = new GraphicsDocumentStore(DocumentPath); var loaded = await recovery.LoadAsync();
        Assert.True(loaded.RecoveredBackup); Assert.False(loaded.ReadOnly); Assert.Equal("default", loaded.Document.ShowId);
        await recovery.SaveAsync(loaded.Document);
        Assert.Equal("{interrupted", await File.ReadAllTextAsync(DocumentPath + ".corrupt"));
        Assert.Equal("default", (await new GraphicsDocumentStore(DocumentPath + ".bak").LoadAsync()).Document.ShowId);
    }
    [Fact]
    public async Task NewerSchemaStaysReadOnlyAndExternalEditsCannotBeOverwritten()
    {
        Directory.CreateDirectory(_directory); var newer = "{\"schemaVersion\":4,\"future\":true}";
        await File.WriteAllTextAsync(DocumentPath, newer);
        var store = new GraphicsDocumentStore(DocumentPath);
        Assert.True((await store.LoadAsync()).ReadOnly);
        await Assert.ThrowsAsync<InvalidOperationException>(() => store.SaveAsync(Example));
        Assert.Equal(newer, await File.ReadAllTextAsync(DocumentPath));
        File.Delete(DocumentPath); store = new(DocumentPath); await store.SaveAsync(Example);
        await File.WriteAllTextAsync(DocumentPath, "external edit");
        await Assert.ThrowsAsync<IOException>(() => store.SaveAsync(Example));
        Assert.Equal("external edit", await File.ReadAllTextAsync(DocumentPath));
    }
    [Theory]
    [InlineData("{\"schemaVersion\":1,\"schemaVersion\":1}")]
    [InlineData("{\"schemaVersion\":1,\"cues\":null}")]
    [InlineData("{\"schemaVersion\":1,\"cues\":[{\"id\":\"x\",\"name\":null}]}")]
    public async Task InvalidDocumentsAreRetainedAndNeverReplaceableByAnEmptyWorkspace(string contents)
    {
        Directory.CreateDirectory(_directory); await File.WriteAllTextAsync(DocumentPath, contents);
        var store = new GraphicsDocumentStore(DocumentPath); Assert.True((await store.LoadAsync()).ReadOnly);
        await Assert.ThrowsAsync<InvalidOperationException>(() => store.SaveAsync(Example));
        Assert.Equal(contents, await File.ReadAllTextAsync(DocumentPath));
    }
    [Fact]
    public async Task PlaylistRoundTripsWithHotkeysAndPins()
    {
        var store = new GraphicsDocumentStore(DocumentPath);
        await store.SaveAsync(Example);
        Assert.Empty((await new GraphicsDocumentStore(DocumentPath).LoadAsync()).Document.Playlist);
        Assert.DoesNotContain("instanceId", await File.ReadAllTextAsync(DocumentPath));
        GraphicsPlaylistRow[] rows = [
            new() { Id = "guest", Kind = GraphicsPlaylistRow.Guest, Name = "Guest name", Hotkey = "F1", PlayOutAfterMs = 8000, TemplateId = "minimal-accent", TemplateVersion = 1,
                Fields = new Dictionary<string, JsonElement> { ["name"] = JsonSerializer.SerializeToElement("Alex Rivera"), ["title"] = JsonSerializer.SerializeToElement("") }, Style = LowerThirdAppearance.FromPreset("minimal-accent"), FollowSources = true },
            new() { Id = "live-bug", Kind = GraphicsPlaylistRow.LiveBug, Name = "LIVE bug", Hotkey = "F4", TemplateId = "clock-panel", TemplateVersion = 3, Fields = new Dictionary<string, JsonElement> { ["label"] = JsonSerializer.SerializeToElement("LIVE") }, ShowClock = true },
            new() { Id = "sponsor", Kind = GraphicsPlaylistRow.Sponsor, Name = "Sponsor card", Hotkey = "F5", PlayOutAfterMs = 15000, TemplateId = "graphic-abc", TemplateVersion = 1, ImagePath = @"C:\show\sponsor.png", Image = new("graphic-abc", 1, "logo") },
            new() { Id = "title", Kind = GraphicsPlaylistRow.Title, Name = "Unassigned", TemplateId = "centered-title", TemplateVersion = 1 }];
        await store.SaveAsync(Example with { Playlist = rows });
        var loaded = (await new GraphicsDocumentStore(DocumentPath).LoadAsync()).Document;
        Assert.Equal(new[] { "F1", "F4", "F5", "" }, loaded.Playlist.Select(r => r.Hotkey));
        Assert.Equal(new[] { ("minimal-accent", 1), ("clock-panel", 3), ("graphic-abc", 1), ("centered-title", 1) }, loaded.Playlist.Select(r => (r.TemplateId, r.TemplateVersion)));
        Assert.Equal(new[] { 8000, 0, 15000, 0 }, loaded.Playlist.Select(r => r.PlayOutAfterMs));
        Assert.True(loaded.Playlist[0].FollowSources); Assert.Equal("", loaded.Playlist[0].Fields["title"].GetString()); Assert.Equal(0, loaded.Playlist[0].Style.BackgroundOpacity);
        Assert.True(loaded.Playlist[1].ShowClock); Assert.Equal("LIVE", loaded.Playlist[1].Fields["label"].GetString());
        Assert.Equal(new NativeGraphicsImageReference("graphic-abc", 1, "logo"), loaded.Playlist[2].Image); Assert.Equal(@"C:\show\sponsor.png", loaded.Playlist[2].ImagePath);
        var text = await File.ReadAllTextAsync(DocumentPath);
        Assert.DoesNotContain("instanceId", text); Assert.DoesNotContain("revision", text); Assert.DoesNotContain("visibility", text);
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Playlist = [rows[0], rows[0]] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Playlist = [rows[0] with { Hotkey = "F13" }] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Playlist = [rows[0] with { PlayOutAfterMs = 5000 }] }));
    }
    // #1076 slice 2: shot layouts persist additively (since schema 2); a document saved before
    // they existed loads them empty, and the validator refuses malformed ones whole.
    [Fact]
    public async Task ShotLayoutsRoundTripAndOlderDocumentsLoadEmpty()
    {
        var store = new GraphicsDocumentStore(DocumentPath);
        await store.SaveAsync(Example);
        var before = await new GraphicsDocumentStore(DocumentPath).LoadAsync();
        Assert.Empty(before.Document.ShotLayouts); Assert.Equal(3, before.Document.SchemaVersion);
        ShotLayout[] layouts = [
            new() { Id = "open", Name = "Open", SceneId = "intro", RowIds = ["title", "logo"], Hotkey = "Shift+F1" },
            new() { Id = "clean", Name = "Clean", SceneId = "", RowIds = [], Hotkey = "Shift+F11", PlayOutOthers = true },
            new() { Id = "unkeyed", Name = "No key", SceneId = "panel", RowIds = ["guest"] }];
        Assert.All(layouts, l => Assert.True(l.IsValid));
        await store.SaveAsync(Example with { ShotLayouts = layouts });
        var loaded = (await new GraphicsDocumentStore(DocumentPath).LoadAsync()).Document;
        Assert.Equal(3, loaded.SchemaVersion);
        Assert.Equal(new[] { "open", "clean", "unkeyed" }, loaded.ShotLayouts.Select(l => l.Id));
        Assert.Equal(new[] { "Shift+F1", "Shift+F11", "" }, loaded.ShotLayouts.Select(l => l.Hotkey));
        Assert.Equal(new[] { "title", "logo" }, loaded.ShotLayouts[0].RowIds); Assert.Equal("intro", loaded.ShotLayouts[0].SceneId);
        Assert.True(loaded.ShotLayouts[1].PlayOutOthers); Assert.Empty(loaded.ShotLayouts[1].RowIds); Assert.Equal("", loaded.ShotLayouts[1].SceneId);
        Assert.False(loaded.ShotLayouts[2].PlayOutOthers);
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { ShotLayouts = [layouts[0], layouts[0]] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { ShotLayouts = [layouts[0] with { Hotkey = "Shift+F12" }] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { ShotLayouts = [layouts[0] with { Hotkey = "F1" }] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { ShotLayouts = [layouts[0] with { RowIds = ["title", "title"] }] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { ShotLayouts = [layouts[0] with { Name = "" }] }));
        Assert.False(new ShotLayout { Id = "Bad Id", Name = "x" }.IsValid);
    }
    private const string V2Document = """
    {"schemaVersion":2,"showId":"default","authority":"legacy",
     "cues":[{"id":"cue-one","name":"Interview","scope":"global","sceneId":"","sourceId":"","draft":{"templateId":"compact-solid","templateVersion":1,"fields":{"name":"Ada","title":""},"style":{},"animations":{"in":{"type":"none","direction":"left","easing":"linear","durationMs":0},"out":{"type":"none","direction":"left","easing":"linear","durationMs":0}}}}],
     "templates":[{"templateId":"graphic-abc","templateVersion":1,"manifestHash":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","definition":{"templateId":"graphic-abc","templateVersion":1},"packageRoot":""}],
     "playlist":[
      {"id":"guest","kind":"guest","name":"Guest name","hotkey":"F1","playOutAfterMs":8000,"templateId":"minimal-accent","templateVersion":1,"fields":{"name":"Alex Rivera","title":""},"followSources":true,
       "motion":{"in":{"type":"slide","direction":"left","easing":"ease-out","durationMs":300},"out":{"type":"fade","direction":"left","easing":"linear","durationMs":200}}},
      {"id":"names","kind":"title","name":"Names list","hotkey":"F2","playOutAfterMs":0,"templateId":"graphic-abc","templateVersion":1,"fields":{},
       "variants":[{"id":"ada","name":"Ada Lovelace","title":"Analyst"},{"id":"grace","name":"Grace Hopper","title":""}],
       "placement":{"anchor":"lower-left","x":96,"y":-54,"scale":1.25},
       "clock":{"mode":"countdown","zone":"local","format":"hms","finish":"hold-zero","offsetMinutes":0,"durationSeconds":600}}],
     "shotLayouts":[{"id":"open","name":"Open","sceneId":"intro","rowIds":["guest","names"],"hotkey":"Shift+F1","playOutOthers":true}],
     "stylePresets":{"Evening":{"nameSize":64}}}
    """;
    private static GraphicsDesign Design(string id, string name = "Panel name bar", string? templateId = null) =>
        new(id, name, "lower-third", templateId ?? "design-panel-name-bar-a1b2c3", 1, JsonSerializer.SerializeToElement(new { templateId = templateId ?? "design-panel-name-bar-a1b2c3", templateVersion = 1 }),
            [new("logo", new string('a', 64), "assets/logo.png")], DateTimeOffset.Parse("2026-11-01T10:00:00Z"));
    [Fact]
    public async Task SchemaTwoDocumentLoadsAsSchemaThreeWithNoDesignsAndEverythingElseIntact()
    {
        Directory.CreateDirectory(_directory); await File.WriteAllTextAsync(DocumentPath, V2Document);
        var loaded = await new GraphicsDocumentStore(DocumentPath).LoadAsync();
        Assert.False(loaded.ReadOnly); var d = loaded.Document;
        Assert.Equal(3, d.SchemaVersion); Assert.Empty(d.Designs);
        Assert.Equal("cue-one", Assert.Single(d.Cues).Id); Assert.Equal("graphic-abc", Assert.Single(d.Templates).TemplateId);
        Assert.Equal(new[] { "guest", "names" }, d.Playlist.Select(r => r.Id));
        Assert.True(d.Playlist[0].FollowSources); Assert.Equal("slide", d.Playlist[0].Motion!.In.Type); Assert.Equal(8000, d.Playlist[0].PlayOutAfterMs);
        Assert.Equal(new[] { "ada", "grace" }, d.Playlist[1].Variants.Select(v => v.Id)); Assert.Equal(1.25, d.Playlist[1].Placement!.Scale);
        Assert.Equal("countdown", d.Playlist[1].Clock!.Mode); Assert.Equal(600, d.Playlist[1].Clock!.DurationSeconds);
        Assert.Equal(new[] { "guest", "names" }, Assert.Single(d.ShotLayouts).RowIds); Assert.True(d.ShotLayouts[0].PlayOutOthers);
        Assert.Equal(64, d.StylePresets["Evening"].NameSize);
        Assert.Equal(V2Document, await File.ReadAllTextAsync(DocumentPath));
    }
    [Fact]
    public async Task FirstSchemaThreeSaveKeepsTheV2FileOnceAndNeverOverwritesIt()
    {
        Directory.CreateDirectory(_directory); await File.WriteAllTextAsync(DocumentPath, V2Document);
        var store = new GraphicsDocumentStore(DocumentPath); var loaded = await store.LoadAsync();
        Assert.False(File.Exists(DocumentPath + ".v2.bak"));
        await store.SaveAsync(loaded.Document with { Designs = [Design("d1")] });
        Assert.Equal(V2Document, await File.ReadAllTextAsync(DocumentPath + ".v2.bak"));
        Assert.Contains("\"schemaVersion\": 3", await File.ReadAllTextAsync(DocumentPath));
        await store.SaveAsync(loaded.Document with { Designs = [Design("d1"), Design("d2", "Second", "design-second-ffffff")] });
        Assert.Equal(V2Document, await File.ReadAllTextAsync(DocumentPath + ".v2.bak"));
        // A fresh store over the now-v3 file never makes another copy.
        var again = new GraphicsDocumentStore(DocumentPath); await again.SaveAsync((await again.LoadAsync()).Document);
        Assert.Equal(V2Document, await File.ReadAllTextAsync(DocumentPath + ".v2.bak"));
        // A v1 file next to an existing .v2.bak leaves that .bak untouched.
        File.Delete(DocumentPath); await File.WriteAllTextAsync(DocumentPath, V2Document.Replace("\"schemaVersion\":2", "\"schemaVersion\":1"));
        await File.WriteAllTextAsync(DocumentPath + ".v2.bak", "earlier backup");
        var third = new GraphicsDocumentStore(DocumentPath); await third.SaveAsync((await third.LoadAsync()).Document);
        Assert.Equal("earlier backup", await File.ReadAllTextAsync(DocumentPath + ".v2.bak"));
    }
    [Fact]
    public async Task FreshWorkspaceSavesWithoutAV2Backup()
    {
        await new GraphicsDocumentStore(DocumentPath).SaveAsync(Example);
        Assert.False(File.Exists(DocumentPath + ".v2.bak"));
    }
    [Fact]
    public async Task DesignsRoundTripAndAreValidated()
    {
        var store = new GraphicsDocumentStore(DocumentPath);
        var designs = new[] { Design("d1"), Design("d2", "Second", "design-second-ffffff") };
        await store.SaveAsync(Example with { Designs = designs });
        var loaded = (await new GraphicsDocumentStore(DocumentPath).LoadAsync()).Document;
        Assert.Equal(new[] { "d1", "d2" }, loaded.Designs.Select(d => d.DesignId));
        Assert.Equal("design-panel-name-bar-a1b2c3", loaded.Designs[0].TemplateId); Assert.Equal("lower-third", loaded.Designs[0].Family);
        Assert.Equal(1, loaded.Designs[0].Definition.GetProperty("templateVersion").GetInt32());
        Assert.Equal(new GraphicsDesignAsset("logo", new string('a', 64), "assets/logo.png"), Assert.Single(loaded.Designs[0].Assets));
        Assert.Equal(designs[0].UpdatedAt, loaded.Designs[0].UpdatedAt);
        var many = Enumerable.Range(0, 32).Select(i => Design("d" + i, "N" + i, "design-n-" + i.ToString("x6"))).ToArray();
        await store.SaveAsync(Example with { Designs = many });
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Designs = many.Append(Design("d33", "N33", "design-n-000033")).ToArray() }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Designs = [Design("d1"), Design("d1", "Other", "design-other-aaaaaa")] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Designs = [Design("d1", templateId: "compact-solid")] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Designs = [Design("d1", templateId: "Design-Bad-a1b2c3")] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Designs = [Design("d1", name: "")] }));
        await Assert.ThrowsAsync<InvalidDataException>(() => store.SaveAsync(Example with { Designs = [Design("d1", name: new string('x', 81))] }));
        await store.SaveAsync(Example with { Designs = [Design("d1", name: new string('x', 80))] });
    }
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
