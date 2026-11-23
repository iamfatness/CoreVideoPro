using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 7, the reverse direction: a show document the MAC wrote (dataset + data
/// design added to the Windows show by ShellTestsGraphicsData.testMacWritesADataSourceWindowsCanRead
/// in write mode) must load in the real Windows store, valid, with nothing lost.</summary>
public sealed class GraphicsShellMacDocumentTests
{
    [Fact]
    public async Task MacWrittenDataDocumentLoadsAndValidates()
    {
        var source = GraphicsShellVectorTests.DocumentPath("mac-data-roundtrip.json");
        Assert.True(File.Exists(source), $"{source} is missing; run the Mac shell suite with COREVIDEO_WRITE_GRAPHICS_SHELL_VECTORS=1.");
        var directory = Path.Combine(Path.GetTempPath(), "cvp-mac-document-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var path = Path.Combine(directory, "default.json");
            File.Copy(source, path);
            var load = await new GraphicsDocumentStore(path).LoadAsync();
            Assert.False(load.ReadOnly, load.Status);
            var dataSource = Assert.Single(load.Document.DataSources);
            Assert.True(dataSource.Valid());
            Assert.Equal("mac-scores", dataSource.Id);
            Assert.Equal(2026, dataSource.ReceivedAt.Year);
            var design = Assert.Single(load.Document.Designs);
            Assert.Equal("design-mac-scores-0a1b2c", design.TemplateId);
            Assert.NotNull(GraphicsDataTable.Binding(design.Definition));
            Assert.Contains(load.Document.Playlist, row => row.DataBinding?.SourceId == "mac-scores");
            Assert.Equal(5, load.Document.Playlist.Count(row => row.DataBinding is null));
        }
        finally { Directory.Delete(directory, true); }
    }
}
