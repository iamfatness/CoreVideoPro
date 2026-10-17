using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsPackageInput(string Manifest, string PackageRoot);
public interface IGraphicsPackageReader
{
    Task<GraphicsPackageInput> ReadAsync(string archivePath, CancellationToken cancellation = default);
}

/// <summary>Explicit local ZIP input adapter, never a media/animation owner.
/// Expansion runs on a worker and writes content-addressed, inert image assets.
/// The native registry independently validates the definition, paths and hashes.
/// No archive path is extracted, evaluated, loaded as code or opened as a URL.</summary>
public sealed class GraphicsPackageReader : IGraphicsPackageReader
{
    private const long ArchiveLimit = 32L * 1024 * 1024, ExpandedLimit = 128L * 1024 * 1024, CacheLimit = 512L * 1024 * 1024;
    private readonly string _root;
    private readonly SemaphoreSlim _worker = new(1, 1);
    private static readonly Encoding Utf8 = new UTF8Encoding(false, true);
    public GraphicsPackageReader(string cacheRoot) => _root = Path.GetFullPath(cacheRoot);
    public async Task<GraphicsPackageInput> ReadAsync(string archivePath, CancellationToken cancellation = default)
    {
        if (!await _worker.WaitAsync(0, cancellation).ConfigureAwait(false)) throw new InvalidOperationException("A graphics package import is already running.");
        try { return await Task.Run(() => Read(archivePath, cancellation), cancellation).ConfigureAwait(false); }
        finally { _worker.Release(); }
    }
    private GraphicsPackageInput Read(string path, CancellationToken cancellation)
    {
        var fullPath = Path.GetFullPath(path); NoReparse(fullPath);
        using var file = new FileStream(fullPath, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, FileOptions.SequentialScan);
        if (file.Length > ArchiveLimit) throw new InvalidDataException("Graphics package exceeds 32 MiB compressed.");
        using var archive = new ZipArchive(file, ZipArchiveMode.Read, false, Utf8);
        if (archive.Entries.Count > 256) throw new InvalidDataException("Graphics package has too many entries.");
        var entries = new Dictionary<string, ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
        long expanded = 0;
        foreach (var entry in archive.Entries) {
            cancellation.ThrowIfCancellationRequested();
            var name = entry.FullName;
            if (name.Length == 0 || name.Length > 200 || name.Contains('\\') || name.Contains(':') || name.StartsWith('/') ||
                name.Split('/').Any(p => p is "." or ".." || p.EndsWith(' ') || p.EndsWith('.')))
                throw new InvalidDataException("Unsafe graphics package path.");
            var unixType = (entry.ExternalAttributes >> 16) & 0xf000;
            if (unixType is not (0 or 0x8000 or 0x4000) || (entry.ExternalAttributes & 0x400) != 0)
                throw new InvalidDataException("Graphics packages cannot contain links or special files.");
            if (name.EndsWith('/')) {
                if (name != "assets/" || entry.Length != 0) throw new InvalidDataException("Unexpected package directory.");
                continue;
            }
            if (unixType == 0x4000 || !entries.TryAdd(name, entry) || entries.Count > 128) throw new InvalidDataException("Duplicate or oversized graphics package.");
            if (entry.Length < 0 || entry.Length > ExpandedLimit - expanded) throw new InvalidDataException("Graphics package exceeds 128 MiB expanded.");
            expanded += entry.Length;
        }
        if (!entries.TryGetValue("template.json", out var manifestEntry) || manifestEntry.FullName != "template.json" || manifestEntry.Length > 256 * 1024)
            throw new InvalidDataException("A package needs a root template.json of at most 256 KiB.");
        byte[] manifestBytes;
        using (var stream = manifestEntry.Open()) {
            using var memory = new MemoryStream(); CopyBounded(stream, memory, 256 * 1024, cancellation); manifestBytes = memory.ToArray();
        }
        var manifest = Utf8.GetString(manifestBytes);
        using var json = JsonDocument.Parse(manifest, new JsonDocumentOptions { MaxDepth = 32 });
        if (json.RootElement.ValueKind != JsonValueKind.Object) throw new InvalidDataException("Invalid graphics manifest.");
        UniqueKeys(json.RootElement);
        if (!json.RootElement.TryGetProperty("schemaVersion", out var schema) || schema.ValueKind != JsonValueKind.Number || schema.GetDouble() != 1)
            throw new InvalidDataException("Unsupported graphics package schema.");
        var assets = new List<(string File, string Hash)>(); var declared = new HashSet<string>(StringComparer.Ordinal);
        if (json.RootElement.TryGetProperty("assets", out var declarations)) {
            if (declarations.ValueKind != JsonValueKind.Array || declarations.GetArrayLength() > 127) throw new InvalidDataException("Invalid package assets.");
            foreach (var declaration in declarations.EnumerateArray()) {
                if (declaration.ValueKind != JsonValueKind.Object || !declaration.TryGetProperty("file", out var assetFile) || assetFile.ValueKind != JsonValueKind.String ||
                    !declaration.TryGetProperty("sha256", out var assetHash) || assetHash.ValueKind != JsonValueKind.String) throw new InvalidDataException("An asset needs a file and SHA-256.");
                var name = assetFile.GetString()!; var hash = assetHash.GetString()!;
                var extension = Path.GetExtension(name);
                if (hash.Length != 64 || hash.Any(c => c is not (>= '0' and <= '9' or >= 'a' and <= 'f')) ||
                    extension is not (".png" or ".jpg" or ".jpeg") || name != "assets/" + hash + extension || !declared.Add(name))
                    throw new InvalidDataException("Assets must use content-hashed local PNG/JPEG paths.");
                if (!entries.ContainsKey(name)) {
                    if (declaration.TryGetProperty("optional", out var optional) && optional.ValueKind == JsonValueKind.True) continue;
                    throw new InvalidDataException("Required package asset is missing: " + name);
                }
                assets.Add((name, hash));
            }
        }
        if (entries.Keys.Any(name => name != "template.json" && !declared.Contains(name))) throw new InvalidDataException("Undeclared files, nested archives and executable assets are not permitted.");
        NoReparse(_root); Directory.CreateDirectory(_root); NoReparse(_root);
        var assetRoot = Path.Combine(_root, "assets"); NoReparse(assetRoot); Directory.CreateDirectory(assetRoot); NoReparse(assetRoot);
        if (Directory.EnumerateDirectories(assetRoot).Any()) throw new InvalidDataException("Unexpected directory in the graphics asset cache.");
        var existing = Directory.EnumerateFiles(assetRoot).Select(p => new FileInfo(p)).ToArray();
        foreach (var item in existing) NoReparse(item.FullName);
        long cached = existing.Sum(f => f.Length); var count = existing.Length;
        long actualExpanded = manifestBytes.Length;
        foreach (var asset in assets) {
            cancellation.ThrowIfCancellationRequested();
            var destination = Path.GetFullPath(Path.Combine(_root, asset.File));
            if (!destination.StartsWith(assetRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Asset path escaped the graphics cache.");
            NoReparse(destination);
            if (File.Exists(destination)) {
                using var previous = File.OpenRead(destination);
                if (!Convert.ToHexString(SHA256.HashData(previous)).Equals(asset.Hash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("A cached graphics asset needs repair.");
                continue;
            }
            var entry = entries[asset.File];
            if (cached > CacheLimit - entry.Length || count >= 2048) throw new InvalidDataException("The graphics asset cache is full; existing assets are preserved.");
            var temporary = Path.Combine(assetRoot, ".graphics-" + Guid.NewGuid().ToString("N") + ".tmp");
            try {
                long bytes;
                using (var input = entry.Open()) using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None)) {
                    bytes = CopyBounded(input, output, Math.Min(ExpandedLimit - actualExpanded, CacheLimit - cached), cancellation);
                    output.Position = 0;
                    if (!Convert.ToHexString(SHA256.HashData(output)).Equals(asset.Hash, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Graphics asset hash mismatch.");
                    output.Flush(true);
                }
                NoReparse(assetRoot); File.Move(temporary, destination); cached += bytes; actualExpanded += bytes; ++count;
            } finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
        return new(manifest, _root);
    }
    private static long CopyBounded(Stream input, Stream output, long limit, CancellationToken cancellation)
    {
        var buffer = new byte[65536]; long total = 0; int read;
        while ((read = input.Read(buffer)) != 0) {
            cancellation.ThrowIfCancellationRequested();
            if (total > limit - read) throw new InvalidDataException("Expanded graphics content exceeds its byte budget.");
            output.Write(buffer, 0, read); total += read;
        }
        return total;
    }
    internal static void NoReparse(string path)
    {
        for (var current = path; !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current)) {
            try { if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0) throw new InvalidDataException("Graphics imports cannot follow links or reparse points."); }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
        }
    }
    private static void UniqueKeys(JsonElement node)
    {
        if (node.ValueKind == JsonValueKind.Object) {
            var keys = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in node.EnumerateObject()) { if (!keys.Add(property.Name)) throw new InvalidDataException("Duplicate graphics manifest key."); UniqueKeys(property.Value); }
        } else if (node.ValueKind == JsonValueKind.Array) foreach (var item in node.EnumerateArray()) UniqueKeys(item);
    }
}
