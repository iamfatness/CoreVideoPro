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
        var store=new GraphicsDocumentStore(DocumentPath);var loaded=await store.LoadAsync();Assert.Equal(2,loaded.Document.SchemaVersion);Assert.Equal(old,await File.ReadAllTextAsync(DocumentPath));
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
        Directory.CreateDirectory(_directory); var newer = "{\"schemaVersion\":3,\"future\":true}";
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
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
