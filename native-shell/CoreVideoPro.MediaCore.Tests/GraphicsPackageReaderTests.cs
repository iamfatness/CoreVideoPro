using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;
public sealed class GraphicsPackageReaderTests : IDisposable
{
    private readonly string _directory = Path.Combine(Path.GetTempPath(), "corevideo-graphics-package-" + Guid.NewGuid().ToString("N"));
    private string Cache => Path.Combine(_directory, "cache");
    private static readonly byte[] Image = Convert.FromBase64String("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScLbtAAAAABJRU5ErkJggg==");
    private static string Hash => Convert.ToHexString(SHA256.HashData(Image)).ToLowerInvariant();
    [Fact]
    public async Task VerifiedPackageExportRoundTripsAndFailedRepairRetainsExistingDestination()
    {
        var source=Package(("template.json",Encoding.UTF8.GetBytes(Manifest),0),("assets/"+Hash+".png",Image,0));
        var imported=await new GraphicsPackageReader(Cache).ReadAsync(source);
        var template=new CoreVideoPro.MediaCore.Models.NativeGraphicsTemplate {Runtime="declarative",PackageRoot=Cache,Definition=JsonDocument.Parse(imported.Manifest).RootElement.Clone()};
        var target=Path.Combine(_directory,"export.cvgraphics");await GraphicsPackageWriter.WriteAsync(template,target);
        var roundtrip=await new GraphicsPackageReader(Path.Combine(_directory,"roundtrip")).ReadAsync(target);Assert.Equal(Manifest,roundtrip.Manifest);
        Assert.Equal(Image,await File.ReadAllBytesAsync(Path.Combine(roundtrip.PackageRoot,"assets",Hash+".png")));
        var before=await File.ReadAllBytesAsync(target);await File.WriteAllBytesAsync(Path.Combine(Cache,"assets",Hash+".png"),new byte[32]);
        await Assert.ThrowsAsync<InvalidDataException>(()=>GraphicsPackageWriter.WriteAsync(template,target));Assert.Equal(before,await File.ReadAllBytesAsync(target));
        Assert.Empty(Directory.GetFiles(_directory,".cvgraphics-*.tmp"));
    }
    [Fact]
    public async Task LocalImageStagingIsContentAddressedAndNameIndependentWithoutReplacingCorruptCache()
    {
        Directory.CreateDirectory(_directory);var first=Path.Combine(_directory,"first.png");var second=Path.Combine(_directory,"second.png");
        await File.WriteAllBytesAsync(first,Image);await File.WriteAllBytesAsync(second,Image);
        var a=await GraphicsImageImport.StageAsync(first,Cache);var b=await GraphicsImageImport.StageAsync(second,Cache);
        Assert.Equal(a,b);Assert.Single(Directory.GetFiles(Path.Combine(Cache,"assets")));
        using var manifest=JsonDocument.Parse(a.Package.Manifest);
        Assert.Equal("none",manifest.RootElement.GetProperty("animations").GetProperty("in").GetProperty("type").GetString());
        Assert.Equal("none",manifest.RootElement.GetProperty("animations").GetProperty("out").GetProperty("type").GetString());
        Assert.Equal(Hash,manifest.RootElement.GetProperty("assets")[0].GetProperty("sha256").GetString());
        await File.WriteAllBytesAsync(Path.Combine(Cache,"assets",Hash+".png"),new byte[32]);
        await Assert.ThrowsAsync<InvalidDataException>(()=>GraphicsImageImport.StageAsync(first,Cache));
    }
    private static string Manifest => JsonSerializer.Serialize(new { schemaVersion = 1, metadata = new { note = "Keep me" }, assets = new[] { new { id = "logo", file = "assets/" + Hash + ".png", sha256 = Hash } } });
    private string Package(params (string Name, byte[] Bytes, int Attributes)[] entries)
    {
        Directory.CreateDirectory(_directory); var path = Path.Combine(_directory, Guid.NewGuid().ToString("N") + ".zip");
        using var file = File.Create(path); using var zip = new ZipArchive(file, ZipArchiveMode.Create);
        foreach (var entry in entries) { var item = zip.CreateEntry(entry.Name); item.ExternalAttributes = entry.Attributes; using var output = item.Open(); output.Write(entry.Bytes); }
        return path;
    }
    [Fact]
    public async Task ContentAddressedImagesAndMetadataRoundTripWithoutAnyNativeOrProgramOperation()
    {
        var path = Package(("template.json", Encoding.UTF8.GetBytes(Manifest), 0), ("assets/" + Hash + ".png", Image, 0));
        var reader = new GraphicsPackageReader(Cache); var input = await reader.ReadAsync(path);
        Assert.Equal(Manifest, input.Manifest); Assert.Equal(Path.GetFullPath(Cache), input.PackageRoot);
        var stored = Path.Combine(Cache, "assets", Hash + ".png"); Assert.Equal(Image, await File.ReadAllBytesAsync(stored));
        await reader.ReadAsync(path); Assert.Single(Directory.GetFiles(Path.Combine(Cache, "assets")));
    }
    [Theory]
    [InlineData("../outside.png")]
    [InlineData("C:/outside.png")]
    [InlineData("/absolute.png")]
    [InlineData("assets\\outside.png")]
    [InlineData("assets/../outside.png")]
    [InlineData("payload.exe")]
    [InlineData("nested.zip")]
    [InlineData("Template.json")]
    public async Task UnsafeDuplicateUndeclaredAndExecutableEntriesAreRefused(string member)
    {
        var path = Package(("template.json", Encoding.UTF8.GetBytes("{\"schemaVersion\":1}"), 0), (member, Image, 0));
        await Assert.ThrowsAsync<InvalidDataException>(() => new GraphicsPackageReader(Cache).ReadAsync(path));
        Assert.False(File.Exists(Path.Combine(_directory, "outside.png")));
    }
    [Fact]
    public async Task SymlinksWrongHashesAndUnknownSchemasNeverPublishCacheAssets()
    {
        var reader = new GraphicsPackageReader(Cache);
        var path = Package(("template.json", Encoding.UTF8.GetBytes(Manifest), 0), ("assets/" + Hash + ".png", Image, unchecked((int)0xA1FF0000)));
        await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(path));
        path = Package(("template.json", Encoding.UTF8.GetBytes(Manifest), 0), ("assets/" + Hash + ".png", new byte[] { 1, 2, 3 }, 0));
        await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(path));
        Assert.Empty(Directory.GetFiles(Path.Combine(Cache, "assets")));
        path = Package(("template.json", Encoding.UTF8.GetBytes("{\"schemaVersion\":2}"), 0));
        await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(path));
        path = Package(("template.json", Encoding.UTF8.GetBytes("{\"schemaVersion\":1,\"schemaVersion\":1}"), 0));
        await Assert.ThrowsAsync<InvalidDataException>(() => reader.ReadAsync(path));
    }
    [Fact]
    public async Task CompressedAndExpandedLimitsRefuseBeforeAnyCacheWrite()
    {
        Directory.CreateDirectory(_directory); var oversized = Path.Combine(_directory, "oversized.zip");
        using (var file = File.Create(oversized)) file.SetLength(32L * 1024 * 1024 + 1);
        var error = await Assert.ThrowsAsync<InvalidDataException>(() => new GraphicsPackageReader(Cache).ReadAsync(oversized));
        Assert.Contains("32 MiB", error.Message); Assert.False(Directory.Exists(Cache));
        var bomb = Path.Combine(_directory, "expanded.zip");
        using (var file = File.Create(bomb)) using (var zip = new ZipArchive(file, ZipArchiveMode.Create)) {
            var entry = zip.CreateEntry("large.png", CompressionLevel.Fastest);
            using var output = entry.Open(); var zeros = new byte[65536];
            for (var i = 0; i < 2048; i++) output.Write(zeros);
            output.WriteByte(0);
        }
        error = await Assert.ThrowsAsync<InvalidDataException>(() => new GraphicsPackageReader(Cache).ReadAsync(bomb));
        Assert.Contains("128 MiB", error.Message); Assert.False(Directory.Exists(Cache));
    }
    public void Dispose() { if (Directory.Exists(_directory)) Directory.Delete(_directory, true); }
}
