using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MediaBinServiceTests : IDisposable
{
    private readonly string _mediaRoot;

    public MediaBinServiceTests()
    {
        _mediaRoot = Path.Combine(Path.GetTempPath(), "corevideo-media-bin-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_mediaRoot);
    }

    public void Dispose()
    {
        if (Directory.Exists(_mediaRoot))
        {
            Directory.Delete(_mediaRoot, recursive: true);
        }
    }

    [Theory]
    [InlineData("intro.mp4", "stinger")]
    [InlineData("clip.MOV", "stinger")]
    [InlineData("transition.webm", "stinger")]
    [InlineData("overlay.png", "lower-third")]
    [InlineData("title.JPG", "lower-third")]
    [InlineData("bed.wav", "audio-bed")]
    [InlineData("music.mp3", "audio-bed")]
    [InlineData("voice.m4a", "audio-bed")]
    public void ClassifyFile_MapsSupportedExtensions(string fileName, string expectedKind)
    {
        var kind = MediaBinClassifier.ClassifyFile(fileName, fileName);
        Assert.Equal(expectedKind, kind);
    }

    [Fact]
    public void ClassifyFile_TreatsSlatesFolderPngAsSlate()
    {
        var kind = MediaBinClassifier.ClassifyFile(Path.Combine("slates", "show-open.png"), "show-open.png");
        Assert.Equal("slate", kind);
    }

    [Fact]
    public void ClassifyFile_TreatsSlatePrefixedFilesAsSlate()
    {
        var kind = MediaBinClassifier.ClassifyFile("assets", "slate-break.mp4");
        Assert.Equal("slate", kind);
    }

    [Fact]
    public void ClassifyFile_IgnoresUnsupportedExtensions()
    {
        Assert.Null(MediaBinClassifier.ClassifyFile("readme.txt", "readme.txt"));
    }

    [Fact]
    public void BuildAssetId_IsStableForRelativePath()
    {
        var first = MediaBinClassifier.BuildAssetId("stingers\\brand.mp4");
        var second = MediaBinClassifier.BuildAssetId("stingers/brand.mp4");
        Assert.Equal(first, second);
        Assert.StartsWith("media-", first, StringComparison.Ordinal);
    }

    [Fact]
    public void GroupAssets_OrdersKindsAndOmitsEmptyGroups()
    {
        var assets = new[]
        {
            new MediaBinAssetDescriptor("a", "Bed", "audio-bed", 30000, "audio-bed/bed.wav", Path.Combine(_mediaRoot, "audio-bed", "bed.wav"), "WAV"),
            new MediaBinAssetDescriptor("b", "Stinger", "stinger", 900, "stinger/stinger.mp4", Path.Combine(_mediaRoot, "stinger", "stinger.mp4"), "MP4"),
            new MediaBinAssetDescriptor("c", "Overlay", "lower-third", null, "lower-third/overlay.png", Path.Combine(_mediaRoot, "lower-third", "overlay.png"), "PNG")
        };

        var groups = MediaBinClassifier.GroupAssets(assets);

        Assert.Equal(["stinger", "lower-third", "audio-bed"], groups.Select(group => group.Kind));
        Assert.Equal("Stinger", groups[0].Label);
        Assert.Equal("Audio bed", groups[2].Label);
        Assert.DoesNotContain(groups, group => group.Kind == "slate");
    }

    [Fact]
    public void ScanRoot_LoadsSupportedFilesFromDisk()
    {
        Directory.CreateDirectory(Path.Combine(_mediaRoot, "slates"));
        File.WriteAllText(Path.Combine(_mediaRoot, "brand-stinger.mp4"), "video");
        File.WriteAllText(Path.Combine(_mediaRoot, "lower-third.png"), "image");
        File.WriteAllText(Path.Combine(_mediaRoot, "intro-bed.mp3"), "audio");
        File.WriteAllText(Path.Combine(_mediaRoot, "slates", "opening.png"), "slate");
        File.WriteAllText(Path.Combine(_mediaRoot, "notes.txt"), "skip");

        var assets = MediaBinClassifier.ScanRoot(_mediaRoot);

        Assert.Equal(4, assets.Count);
        Assert.Contains(assets, asset => asset.Kind == "stinger" && asset.Name == "brand-stinger");
        Assert.Contains(assets, asset => asset.Kind == "lower-third" && asset.Name == "lower-third");
        Assert.Contains(assets, asset => asset.Kind == "audio-bed" && asset.Name == "intro-bed");
        Assert.Contains(assets, asset => asset.Kind == "slate" && asset.Name == "opening");
        Assert.Contains(
            assets,
            asset => asset.Id.StartsWith("media-", StringComparison.Ordinal) &&
                     asset.RelativePath == "brand-stinger.mp4" &&
                     asset.FilePath == Path.Combine(_mediaRoot, "brand-stinger.mp4") &&
                     asset.FileType == "MP4");
        Assert.Contains(
            assets,
            asset => asset.RelativePath == Path.Combine("slates", "opening.png") &&
                     asset.FilePath == Path.Combine(_mediaRoot, "slates", "opening.png") &&
                     asset.FileType == "PNG");
        Assert.All(assets, asset => Assert.Null(asset.DurationMs));
    }

    [Fact]
    public void ScanRoot_ReturnsEmptyWhenDirectoryMissing()
    {
        var missing = Path.Combine(_mediaRoot, "missing");
        Assert.Empty(MediaBinClassifier.ScanRoot(missing));
    }

    [Fact]
    public void ScanMediaRoots_DeduplicatesSameRelativeAssetAcrossRoots()
    {
        var packagedRoot = Path.Combine(_mediaRoot, "packaged");
        var localRoot = Path.Combine(_mediaRoot, "local");
        Directory.CreateDirectory(packagedRoot);
        Directory.CreateDirectory(localRoot);
        File.WriteAllText(Path.Combine(packagedRoot, "shared.mp4"), "video");
        File.WriteAllText(Path.Combine(localRoot, "shared.mp4"), "video");

        var assets = MediaBinClassifier.ScanMediaRoots([packagedRoot, localRoot]);

        Assert.Single(assets);
    }

    [Fact]
    public void BuildEmptyGuidanceMessage_ListsConfiguredRoots()
    {
        var localRoot = Path.Combine(_mediaRoot, "local");
        var packagedRoot = Path.Combine(_mediaRoot, "packaged");
        Directory.CreateDirectory(localRoot);
        Directory.CreateDirectory(packagedRoot);

        var guidance = MediaBinClassifier.BuildEmptyGuidanceMessage([localRoot, packagedRoot]);

        Assert.Contains("stingers", guidance, StringComparison.OrdinalIgnoreCase);
        Assert.Contains(localRoot, guidance, StringComparison.Ordinal);
        Assert.Contains(packagedRoot, guidance, StringComparison.Ordinal);
    }
}
