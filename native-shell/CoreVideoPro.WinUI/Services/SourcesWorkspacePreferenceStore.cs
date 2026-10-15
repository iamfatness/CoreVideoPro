using System.Text.Json;

namespace CoreVideoPro.WinUI.Services;

public sealed record SourcesWorkspacePreferences
{
    public bool ShowType { get; init; } = true;
    public bool ShowSignal { get; init; } = true;
    public bool ShowAudio { get; init; } = true;
}

public interface ISourcesWorkspacePreferenceStore
{
    SourcesWorkspacePreferences Load();
    void Save(SourcesWorkspacePreferences preferences);
}

public sealed class SourcesWorkspacePreferenceStore(string path) : ISourcesWorkspacePreferenceStore
{
    private readonly AtomicJsonFile _file = new(path);
    public SourcesWorkspacePreferenceStore() : this(Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "CoreVideoPro", "sources-workspace.json")) { }

    public SourcesWorkspacePreferences Load()
    {
        try { return _file.Locked(() => JsonSerializer.Deserialize<SourcesWorkspacePreferences>(File.ReadAllText(_file.Path)) ?? new()); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        { return new(); }
    }
    public void Save(SourcesWorkspacePreferences preferences) => _file.Locked(() =>
    {
        _file.Write(JsonSerializer.Serialize(preferences), null);
        return true;
    });
}
