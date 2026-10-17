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
        Directory.CreateDirectory(_directory); var newer = "{\"schemaVersion\":2,\"future\":true}";
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
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
