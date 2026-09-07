using System.Text.Json;
using System.Text.Json.Nodes;

namespace CoreVideoPro.ShowEngine;

/// <summary>Reads/writes <c>ohg-show-config.json</c> (spec §9). Mirrors
/// <c>ProductionOutputPreferencesStore</c>'s shape (atomic write, loud parse failure) without
/// referencing WinUI: an app-independent settings store, tested with plain temp directories.</summary>
public sealed class ShowConfigStore
{
    public const string DefaultFileName = "ohg-show-config.json";
    public const string DefaultStateFileName = "ohg-show-state.json";

    public string FilePath { get; }

    public ShowConfigStore(string folderPath, string? fileName = null)
    {
        FilePath = Path.Combine(folderPath, fileName ?? DefaultFileName);
    }

    public bool Exists => File.Exists(FilePath);

    /// <summary>Returns the parsed config, or null + <paramref name="error"/> for: a missing file,
    /// invalid JSON, an unsupported <c>version</c>, or a missing/non-object <c>engine</c> block.</summary>
    public ShowConfig? Load(out string? error)
    {
        if (!File.Exists(FilePath))
        {
            error = $"ohg show config not found: {FilePath}";
            return null;
        }

        string json;
        try
        {
            json = File.ReadAllText(FilePath);
        }
        catch (IOException ex)
        {
            error = $"ohg show config not readable: {ex.Message}";
            return null;
        }
        catch (UnauthorizedAccessException ex)
        {
            error = $"ohg show config not readable: {ex.Message}";
            return null;
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(json);
        }
        catch (JsonException ex)
        {
            error = $"ohg show config invalid JSON: {ex.Message}";
            return null;
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                error = "ohg show config invalid JSON: expected an object";
                return null;
            }

            var version = root.TryGetProperty("version", out var versionElement) &&
                          versionElement.ValueKind == JsonValueKind.Number &&
                          versionElement.TryGetInt32(out var parsedVersion)
                ? parsedVersion
                : 0;

            if (version != ShowConfig.CurrentVersion)
            {
                error = $"unsupported ohg-show-config version {version}";
                return null;
            }

            if (!root.TryGetProperty("engine", out var engineElement) || engineElement.ValueKind != JsonValueKind.Object)
            {
                error = "ohg show config missing 'engine' object";
                return null;
            }

            var shell = new ShowShellConfig();
            if (root.TryGetProperty("shell", out var shellElement) && shellElement.ValueKind == JsonValueKind.Object)
            {
                shell = JsonSerializer.Deserialize<ShowShellConfig>(shellElement.GetRawText(), ShowEngineProtocol.Json)
                        ?? new ShowShellConfig();
            }

            error = null;
            return new ShowConfig
            {
                Version = version,
                Engine = engineElement.Clone(),
                Shell = shell
            };
        }
    }

    /// <summary>Atomic write: serialize, write <c>&lt;path&gt;.tmp</c>, then
    /// <see cref="File.Move(string, string, bool)"/> over the real path. Never leaves a
    /// <c>.tmp</c> file behind, and creates the target folder if needed.</summary>
    public void Save(ShowConfig config)
    {
        var directory = Path.GetDirectoryName(FilePath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var json = JsonSerializer.Serialize(config, ShowEngineProtocol.Json);
        var temporaryPath = FilePath + ".tmp";
        File.WriteAllText(temporaryPath, json);
        File.Move(temporaryPath, FilePath, overwrite: true);
    }

    /// <summary>Ensures <c>engine.statePath</c> is set (default:
    /// <c>&lt;folder&gt;\ohg-show-state.json</c>, where folder is this store's own folder) and
    /// returns the JSON text of the WHOLE config document — the engine host
    /// (<c>show-engine/src/host/main.ts</c>) accepts either the raw engine object or
    /// <c>{ engine: {...} }</c>, so it unwraps this text itself.</summary>
    public string MaterializeEngineConfig(ShowConfig config)
    {
        var engineNode = JsonNode.Parse(config.Engine.GetRawText()) as JsonObject ?? new JsonObject();

        if (!HasNonEmptyStatePath(engineNode))
        {
            var folder = Path.GetDirectoryName(FilePath);
            var statePath = string.IsNullOrEmpty(folder)
                ? DefaultStateFileName
                : Path.Combine(folder, DefaultStateFileName);
            engineNode["statePath"] = statePath;
        }

        var shellNode = JsonNode.Parse(JsonSerializer.Serialize(config.Shell, ShowEngineProtocol.Json));

        var root = new JsonObject
        {
            ["version"] = config.Version,
            ["engine"] = engineNode,
            ["shell"] = shellNode
        };

        return root.ToJsonString(ShowEngineProtocol.Json);
    }

    private static bool HasNonEmptyStatePath(JsonObject engineNode)
    {
        if (!engineNode.TryGetPropertyValue("statePath", out var statePathNode) || statePathNode is null)
        {
            return false;
        }

        return statePathNode.GetValueKind() == JsonValueKind.String &&
               !string.IsNullOrEmpty(statePathNode.GetValue<string>());
    }
}
