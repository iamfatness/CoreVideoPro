using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsSavedCue
{
    public string Id { get; init; } = "";
    public string Name { get; init; } = "";
    public string Scope { get; init; } = "global";
    public string SceneId { get; init; } = "";
    public string SourceId { get; init; } = "";
    public NativeGraphicsDraft? Draft { get; init; }
    public NativeGraphicsClockConfiguration? ClockConfiguration { get; init; }
    [JsonExtensionData] public Dictionary<string, JsonElement>? Metadata { get; init; }
}

public sealed record GraphicsSavedTemplate(string TemplateId, int TemplateVersion, string ManifestHash, JsonElement Definition, string PackageRoot = "");

public sealed record GraphicsStudioLowerThird(GraphicsSavedCue Cue, bool FollowProgramSource);

/// <summary>Separate graphics document. No stream, recording, camera or media
/// preferences belong here. Runtime visibility/arming is deliberately absent.</summary>
public sealed record GraphicsShowDocument
{
    public int SchemaVersion { get; init; } = 2;
    public string ShowId { get; init; } = "default";
    public string Authority { get; init; } = "legacy";
    public IReadOnlyList<GraphicsSavedCue> Cues { get; init; } = [];
    public IReadOnlyList<GraphicsSavedTemplate> Templates { get; init; } = [];
    public GraphicsStudioLowerThird? StudioLowerThird { get; init; }
    public IReadOnlyDictionary<string, LowerThirdAppearance> StylePresets { get; init; } = new Dictionary<string, LowerThirdAppearance>();
    public IReadOnlyDictionary<string, GraphicsSavedStyle> TemplateStyles { get; init; } = new Dictionary<string, GraphicsSavedStyle>();
    public JsonElement? Brand { get; init; }
    public JsonElement? SourceOverrides { get; init; }
    // Configuration only. Native source generations, mode, holds, and arming
    // never survive a restart or an imported document.
    public IReadOnlyList<GraphicsAutomationRule> AutomationRules {get;init;}=[];
    /// <summary>Playout rows (#1076). Additive in schema 2; older documents load it empty.</summary>
    public IReadOnlyList<GraphicsPlaylistRow> Playlist { get; init; } = [];
    [JsonExtensionData] public Dictionary<string, JsonElement>? Metadata { get; init; }
}

public sealed record GraphicsDocumentLoad(GraphicsShowDocument Document, bool ReadOnly, bool RecoveredBackup, string Status);

/// <summary>Atomic, version-guarded graphics persistence. Explicit caller path
/// enables isolated profiles/tests. Never reads or rewrites production preferences.</summary>
public sealed class GraphicsDocumentStore
{
    public const int CurrentSchemaVersion=2;
    public const int ByteLimit = 2 * 1024 * 1024;
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web) { WriteIndented = true, MaxDepth = 32 };
    private readonly string _path;
    private readonly SemaphoreSlim _owner = new(1, 1);
    private bool _loaded, _readOnly, _recovered;
    private string _fingerprint = "";
    public GraphicsDocumentStore(string path) => _path = Path.GetFullPath(path);

    public async Task<GraphicsDocumentLoad> LoadAsync(CancellationToken cancellation = default)
    {
        await _owner.WaitAsync(cancellation).ConfigureAwait(false);
        try { return await Task.Run(Load, cancellation).ConfigureAwait(false); }
        finally { _owner.Release(); }
    }
    private GraphicsDocumentLoad Load()
    {
        _loaded = true; _readOnly = false; _recovered = false;
        try {
            if (!File.Exists(_path) && !File.Exists(_path + ".bak")) { _fingerprint = ""; return new(new(), false, false, "New graphics workspace; automation is unarmed."); }
            var primary = Read(_path); _fingerprint = Fingerprint(primary);
            if (primary is not null) {
                try { return Decode(primary, false); }
                catch (Exception ex) when (InvalidDocument(ex)) { /* Keep primary intact; try the last complete backup. */ }
            }
            var backup = Read(_path + ".bak");
            if (backup is not null) { var result = Decode(backup, true); _recovered = true; return result; }
            _readOnly = true; return new(new(), true, false, "Graphics document is invalid. Original retained; repair or restore a backup before saving.");
        }
        catch (Exception ex) when (InvalidDocument(ex) || ex is IOException or UnauthorizedAccessException) {
            _readOnly = true; return new(new(), true, false, "Graphics document unavailable: " + ex.Message);
        }
    }
    private GraphicsDocumentLoad Decode(byte[] bytes, bool backup)
    {
        using var json = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 32 });
        UniqueKeys(json.RootElement);
        if (!json.RootElement.TryGetProperty("schemaVersion", out var schema) || !schema.TryGetInt32(out var version)) throw new InvalidDataException("Missing graphics schema.");
        if (version > CurrentSchemaVersion) {
            _readOnly = true;
            return new(new(), true, backup, $"Graphics schema {version} is newer than this build. Read-only; the original document is preserved.");
        }
        if (version is not (1 or CurrentSchemaVersion)) throw new InvalidDataException("Unsupported graphics schema.");
        var document = JsonSerializer.Deserialize<GraphicsShowDocument>(bytes, Options) ?? throw new InvalidDataException("Empty graphics document.");
        if(version==1) document=document with {SchemaVersion=CurrentSchemaVersion};
        Validate(document);
        return new(document, false, backup, backup ? "Recovered the last complete graphics backup. Graphics remain hidden and automation unarmed." : "Graphics workspace loaded. Restore cues explicitly; automation is unarmed.");
    }
    public async Task SaveAsync(GraphicsShowDocument document, CancellationToken cancellation = default)
    {
        await _owner.WaitAsync(cancellation).ConfigureAwait(false);
        try {
            await Task.Run(() => {
                if (!_loaded) Load();
                if (_readOnly) throw new InvalidOperationException("Graphics document is read-only; original retained.");
                Validate(document);
                var bytes = JsonSerializer.SerializeToUtf8Bytes(document, Options);
                if (bytes.Length > ByteLimit) throw new InvalidDataException("Graphics document exceeds the 2 MiB limit.");
                if (Fingerprint(Read(_path)) != _fingerprint) throw new IOException("Graphics file changed outside this workspace. Reload before saving.");
                var directory = Path.GetDirectoryName(_path)!; Directory.CreateDirectory(directory);
                var temporary = Path.Combine(directory, ".graphics-" + Guid.NewGuid().ToString("N") + ".tmp");
                try {
                    using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { file.Write(bytes); file.Flush(true); }
                    if (File.Exists(_path)) {
                        if (_recovered) { File.Copy(_path, _path + ".corrupt", true); File.Move(temporary, _path, true); }
                        else File.Replace(temporary, _path, _path + ".bak");
                    } else File.Move(temporary, _path);
                    _fingerprint = Fingerprint(bytes); _recovered = false;
                } finally { if (File.Exists(temporary)) File.Delete(temporary); }
            }, cancellation).ConfigureAwait(false);
        } finally { _owner.Release(); }
    }
    private static byte[]? Read(string path)
    {
        if (!File.Exists(path)) return null;
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete);
        if (file.Length > ByteLimit) throw new InvalidDataException("Graphics document exceeds the 2 MiB limit.");
        var bytes = new byte[checked((int)file.Length)]; file.ReadExactly(bytes); return bytes;
    }
    private static string Fingerprint(byte[]? bytes) => bytes is null ? "" : Convert.ToHexString(SHA256.HashData(bytes));
    private static bool InvalidDocument(Exception ex) => ex is JsonException or InvalidDataException or InvalidOperationException or ArgumentException;
    private static void UniqueKeys(JsonElement node)
    {
        if (node.ValueKind == JsonValueKind.Object) {
            var keys = new HashSet<string>(StringComparer.Ordinal);
            foreach (var property in node.EnumerateObject()) { if (!keys.Add(property.Name)) throw new InvalidDataException("Duplicate graphics key."); UniqueKeys(property.Value); }
        } else if (node.ValueKind == JsonValueKind.Array) foreach (var item in node.EnumerateArray()) UniqueKeys(item);
    }
    private static void Validate(GraphicsShowDocument document)
    {
        if (document.StudioLowerThird is { } assigned) {
            if (assigned.Cue is null || assigned.Cue.Draft?.Binding is not null)
                throw new InvalidDataException("Studio look must not persist a live source generation.");
            Validate(document with { StudioLowerThird = null, Cues = [assigned.Cue], AutomationRules = [], Playlist = [] });
        }
        if (document.SchemaVersion != CurrentSchemaVersion || string.IsNullOrWhiteSpace(document.ShowId) || document.ShowId.Length > 64 ||
            document.Authority is not ("legacy" or "volt") || document.Cues is null || document.Cues.Count > 64 ||
            document.Templates is null || document.Templates.Count > 16 || document.StylePresets is null || document.StylePresets.Count > 64)
            throw new InvalidDataException("Invalid or oversized graphics workspace.");
        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (var cue in document.Cues) {
            if (cue is null || string.IsNullOrEmpty(cue.Id) || cue.Id.Length > 64 || !ids.Add(cue.Id) || cue.Name is null || cue.Name.Length > 128 ||
                cue.Scope is not ("global" or "scene") || (cue.Scope == "scene" && string.IsNullOrEmpty(cue.SceneId)) ||
                cue.SceneId is null || cue.SceneId.Length > 256 || cue.SourceId is null || cue.SourceId.Length > 256 || cue.Draft is not { } draft ||
                string.IsNullOrEmpty(draft.TemplateId) || draft.TemplateId.Length > 64 || draft.TemplateVersion < 1 || draft.Style?.IsValid != true ||
                (cue.ClockConfiguration is not null && !cue.ClockConfiguration.IsValid) || draft.Animations?.In is null || draft.Animations.Out is null || (draft.Placement is not null && !draft.Placement.IsValid) ||
                draft.Fields.ValueKind != JsonValueKind.Object || draft.Fields.EnumerateObject().Count() > 32)
                throw new InvalidDataException("Invalid saved graphics cue.");
            if(draft.Image is { } image && (string.IsNullOrEmpty(image.TemplateId) || image.TemplateId.Length>64 || image.TemplateVersion<1 || string.IsNullOrEmpty(image.AssetId) || image.AssetId.Length>64)) throw new InvalidDataException("Invalid saved image package reference.");
            if(draft.Scratch is { } scratch) {
                if(scratch.Manifest is null || scratch.Hash is not {Length:64} || Encoding.UTF8.GetByteCount(scratch.Manifest)>256*1024 ||
                    !string.Equals(Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(scratch.Manifest))),scratch.Hash,StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Invalid private template checksum.");
                using var edited=JsonDocument.Parse(scratch.Manifest,new JsonDocumentOptions {MaxDepth=32});UniqueKeys(edited.RootElement);
                var root=edited.RootElement;
                if(root.ValueKind!=JsonValueKind.Object || !root.TryGetProperty("templateId",out var id) || id.ValueKind!=JsonValueKind.String || id.GetString()!=draft.TemplateId ||
                    !root.TryGetProperty("templateVersion",out var version) || version.ValueKind!=JsonValueKind.Number || !version.TryGetInt32(out var number) || number!=draft.TemplateVersion || GraphicsTemplateEditor.Layers(scratch).Count>64)
                    throw new InvalidDataException("Private template does not match its pinned parent.");
            }
        }
        foreach (var preset in document.StylePresets)
            if (string.IsNullOrWhiteSpace(preset.Key) || preset.Key.Length > 80 || preset.Value?.IsValid != true) throw new InvalidDataException("Invalid saved graphics style.");
        if(document.TemplateStyles is null || document.TemplateStyles.Count>64) throw new InvalidDataException("Too many saved template styles.");
        if(document.Playlist is null || document.Playlist.Count>64 || document.Playlist.Any(r=>r is null || !r.IsValid) || document.Playlist.Select(r=>r.Id).Distinct(StringComparer.Ordinal).Count()!=document.Playlist.Count) throw new InvalidDataException("Invalid saved graphics playlist.");
        if(document.AutomationRules is null || document.AutomationRules.Count>8 || document.AutomationRules.Any(r=>r is null||!r.IsValid) || document.AutomationRules.Select(r=>r.Id).Distinct(StringComparer.Ordinal).Count()!=document.AutomationRules.Count || document.AutomationRules.Select(r=>r.InstanceId).Distinct(StringComparer.Ordinal).Count()!=document.AutomationRules.Count) throw new InvalidDataException("Invalid saved automation rule configuration.");
        foreach(var preset in document.TemplateStyles)
            if(string.IsNullOrWhiteSpace(preset.Key) || preset.Key.Length>80 || preset.Value is not {Appearance.IsValid:true} style ||
                string.IsNullOrEmpty(style.TemplateId) || style.TemplateId.Length>64 || style.Fields.ValueKind!=JsonValueKind.Object ||
                Encoding.UTF8.GetByteCount(style.Fields.GetRawText())>65536 || style.Fields.EnumerateObject().Count()>32)
                throw new InvalidDataException("Invalid saved template style.");
        foreach (var template in document.Templates)
            if (template is null || string.IsNullOrEmpty(template.TemplateId) || template.TemplateId.Length > 64 || template.TemplateVersion < 1 ||
                template.ManifestHash is not { Length: 64 } || !template.ManifestHash.All(Uri.IsHexDigit) ||
                template.PackageRoot is null || template.PackageRoot.Length > 2048 || template.PackageRoot.Contains('\0') ||
                template.Definition.ValueKind != JsonValueKind.Object || Encoding.UTF8.GetByteCount(template.Definition.GetRawText()) > 256 * 1024)
                throw new InvalidDataException("Invalid saved template document.");
    }
}
