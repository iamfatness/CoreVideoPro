using System.Text.Json;

namespace CoreVideoPro.WinUI.Services;

public sealed record RtmpIngestSourceRecord(int Number, string Url);

public interface IRtmpIngestSourceStore
{
    IReadOnlyList<RtmpIngestSourceRecord> Load();
    void Save(IReadOnlyList<RtmpIngestSourceRecord> sources);
}

public sealed class FileRtmpIngestSourceStore(
    string folder,
    Func<string, string> protect,
    Func<string, string> unprotect) : IRtmpIngestSourceStore
{
    public const string FileName = "rtmp-ingest-sources.json";
    private readonly string _path = Path.Combine(folder, FileName);

    public IReadOnlyList<RtmpIngestSourceRecord> Load()
    {
        if (!File.Exists(_path)) return [];
        try
        {
            var stored = JsonSerializer.Deserialize<List<RtmpIngestSourceRecord>>(File.ReadAllText(_path)) ?? [];
            // Never send DPAPI ciphertext to the native listener when an old
            // machine/account can no longer decrypt a saved source.
            return stored.Select(source => source with
                {
                    Url = string.IsNullOrEmpty(source.Url) ? string.Empty : unprotect(source.Url)
                }).ToList();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or FormatException or System.Security.Cryptography.CryptographicException)
        {
            return [];
        }
    }

    public void Save(IReadOnlyList<RtmpIngestSourceRecord> sources)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        var stored = sources.Select(source => source with
        {
            Url = string.IsNullOrEmpty(source.Url) ? string.Empty : protect(source.Url)
        }).ToList();
        File.WriteAllText(_path, JsonSerializer.Serialize(stored));
    }
}

public sealed class InMemoryRtmpIngestSourceStore : IRtmpIngestSourceStore
{
    private IReadOnlyList<RtmpIngestSourceRecord> _sources = [];
    public IReadOnlyList<RtmpIngestSourceRecord> Load() => _sources;
    public void Save(IReadOnlyList<RtmpIngestSourceRecord> sources) => _sources = sources.ToList();
}
