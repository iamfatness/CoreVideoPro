using System.Text.Json;
using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public class ShowConfigStoreTests : IDisposable
{
    private readonly string _dir;

    public ShowConfigStoreTests()
    {
        _dir = Path.Combine(Path.GetTempPath(), "cvp-show-config-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(_dir);
    }

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
    }

    private static JsonElement Engine(string json) => JsonDocument.Parse(json).RootElement.Clone();

    private ShowConfig SampleConfig() => new()
    {
        Version = 1,
        Engine = Engine("""{"capacity":10,"statePath":""}"""),
        Shell = new ShowShellConfig
        {
            DriveHost = true,
            Presets = new ShowPresetScenes { Solo = "solo-scene", Black = "black-scene" },
            DefaultTransition = "fade",
            TallyUrl = "http://tally.example/"
        }
    };

    [Fact]
    public void Load_MissingFile_ReturnsNullWithNotFoundError()
    {
        var store = new ShowConfigStore(_dir);

        var config = store.Load(out var error);

        Assert.Null(config);
        Assert.Contains("not found", error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Load_InvalidJson_ReturnsNullWithError()
    {
        var store = new ShowConfigStore(_dir);
        File.WriteAllText(store.FilePath, "{ not json");

        var config = store.Load(out var error);

        Assert.Null(config);
        Assert.NotNull(error);
    }

    [Fact]
    public void Load_UnsupportedVersion_ReturnsNullWithErrorMentioningVersion()
    {
        var store = new ShowConfigStore(_dir);
        File.WriteAllText(store.FilePath, """{"version":2,"engine":{"capacity":10}}""");

        var config = store.Load(out var error);

        Assert.Null(config);
        Assert.Equal("unsupported ohg-show-config version 2", error);
    }

    [Fact]
    public void Load_MissingVersionProperty_ReturnsUnsupportedVersion0()
    {
        // Controller ruling (Task 9 fix round 1): a config with no "version" key at all must
        // fail exactly as loudly as an explicit wrong version, not be silently treated as v1.
        var store = new ShowConfigStore(_dir);
        File.WriteAllText(store.FilePath, """{"engine":{"capacity":10}}""");

        var config = store.Load(out var error);

        Assert.Null(config);
        Assert.Equal("unsupported ohg-show-config version 0", error);
    }

    [Fact]
    public void Load_MissingEngineObject_ReturnsNullWithError()
    {
        var store = new ShowConfigStore(_dir);
        File.WriteAllText(store.FilePath, """{"version":1}""");

        var config = store.Load(out var error);

        Assert.Null(config);
        Assert.Contains("engine", error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void Load_EngineIsNotAnObject_ReturnsNullWithError()
    {
        var store = new ShowConfigStore(_dir);
        File.WriteAllText(store.FilePath, """{"version":1,"engine":"nope"}""");

        var config = store.Load(out var error);

        Assert.Null(config);
        Assert.NotNull(error);
    }

    [Fact]
    public void SaveThenLoad_RoundTrips()
    {
        var store = new ShowConfigStore(_dir);
        var original = SampleConfig();

        store.Save(original);
        var loaded = store.Load(out var error);

        Assert.Null(error);
        Assert.NotNull(loaded);
        Assert.Equal(1, loaded!.Version);
        Assert.Equal(10, loaded.Engine.GetProperty("capacity").GetInt32());
        Assert.True(loaded.Shell.DriveHost);
        Assert.Equal("solo-scene", loaded.Shell.Presets.Solo);
        Assert.Equal("black-scene", loaded.Shell.Presets.Black);
        Assert.Null(loaded.Shell.Presets.ActiveSpeaker);
        Assert.Equal("fade", loaded.Shell.DefaultTransition);
        Assert.Equal("http://tally.example/", loaded.Shell.TallyUrl);
    }

    [Fact]
    public void Save_LeavesNoTmpFileBehind()
    {
        var store = new ShowConfigStore(_dir);

        store.Save(SampleConfig());

        Assert.False(File.Exists(store.FilePath + ".tmp"));
        Assert.True(File.Exists(store.FilePath));
    }

    [Fact]
    public void Save_CreatesTargetFolderIfMissing()
    {
        var nestedDir = Path.Combine(_dir, "nested", "folder");
        var store = new ShowConfigStore(nestedDir);

        store.Save(SampleConfig());

        Assert.True(File.Exists(store.FilePath));
    }

    [Fact]
    public void MaterializeEngineConfig_InjectsStatePath_WhenAbsent()
    {
        var store = new ShowConfigStore(_dir);
        var config = new ShowConfig { Engine = Engine("""{"capacity":10}""") };

        var json = store.MaterializeEngineConfig(config);

        using var doc = JsonDocument.Parse(json);
        var statePath = doc.RootElement.GetProperty("engine").GetProperty("statePath").GetString();
        Assert.Equal(Path.Combine(_dir, ShowConfigStore.DefaultStateFileName), statePath);
    }

    [Fact]
    public void MaterializeEngineConfig_InjectsStatePath_WhenEmptyString()
    {
        var store = new ShowConfigStore(_dir);
        var config = new ShowConfig { Engine = Engine("""{"capacity":10,"statePath":""}""") };

        var json = store.MaterializeEngineConfig(config);

        using var doc = JsonDocument.Parse(json);
        var statePath = doc.RootElement.GetProperty("engine").GetProperty("statePath").GetString();
        Assert.Equal(Path.Combine(_dir, ShowConfigStore.DefaultStateFileName), statePath);
    }

    [Fact]
    public void MaterializeEngineConfig_PreservesExistingStatePath()
    {
        var store = new ShowConfigStore(_dir);
        var config = new ShowConfig { Engine = Engine("""{"capacity":10,"statePath":"C:\\custom\\state.json"}""") };

        var json = store.MaterializeEngineConfig(config);

        using var doc = JsonDocument.Parse(json);
        var statePath = doc.RootElement.GetProperty("engine").GetProperty("statePath").GetString();
        Assert.Equal(@"C:\custom\state.json", statePath);
    }

    [Fact]
    public void MaterializeEngineConfig_ReturnsWholeDocument_WithVersionAndShell()
    {
        var store = new ShowConfigStore(_dir);
        var config = SampleConfig();

        var json = store.MaterializeEngineConfig(config);

        using var doc = JsonDocument.Parse(json);
        Assert.Equal(1, doc.RootElement.GetProperty("version").GetInt32());
        Assert.True(doc.RootElement.GetProperty("shell").GetProperty("driveHost").GetBoolean());
    }
}
