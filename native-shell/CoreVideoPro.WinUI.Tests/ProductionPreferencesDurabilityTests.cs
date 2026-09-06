using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ProductionPreferencesDurabilityTests : IDisposable
{
    private readonly string _folder = Path.Combine(Path.GetTempPath(), "corevideo-durable-tests", Guid.NewGuid().ToString("N"));
    private string Primary => Path.Combine(_folder, FileProductionOutputPreferencesStore.DefaultFileName);
    private string Backup => Primary + ".bak";
    private FileProductionOutputPreferencesStore Store() => new(_folder);
    private static ProductionOutputPreferences Prefs(string name) => new() { RecordingFilenamePrefix = name };

    [Fact]
    public void MissingAndCorruptAreDistinct()
    {
        Assert.Equal(ProductionPreferencesLoadStatus.Missing, Store().LoadWithResult().Status);
        Directory.CreateDirectory(_folder);
        File.WriteAllText(Primary, "{truncated");
        Assert.Equal(ProductionPreferencesLoadStatus.Corrupt, Store().LoadWithResult().Status);
        Assert.Null(Store().Load());
    }

    [Fact]
    public void InterruptedReplacementLeavesPreviousShowAndCleansStaging()
    {
        Store().Save(Prefs("previous"));
        var failing = new FileProductionOutputPreferencesStore(_folder, destination =>
        {
            if (destination == Primary) throw new IOException("Injected interruption after flush");
        });
        Assert.Throws<IOException>(() => failing.Save(Prefs("new")));
        Assert.Equal("previous", Store().Load()?.RecordingFilenamePrefix);
        Assert.Equal("previous", ProductionOutputPreferencesSerializer.Deserialize(File.ReadAllText(Backup))?.RecordingFilenamePrefix);
        Assert.Empty(Directory.GetFiles(_folder, "*.tmp"));
    }

    [Fact]
    public void CorruptPrimaryRecoversAndRepairsWithoutOverwritingGoodBackup()
    {
        Store().Save(Prefs("first"));
        Store().Save(Prefs("second"));
        var backup = File.ReadAllText(Backup);
        File.WriteAllText(Primary, "{truncated");
        var result = Store().LoadWithResult();
        Assert.Equal(ProductionPreferencesLoadStatus.Recovered, result.Status);
        Assert.Equal(ProductionPreferencesLoadStatus.Corrupt, result.PrimaryFailure);
        Assert.Equal("first", result.Preferences?.RecordingFilenamePrefix);
        Assert.Equal(backup, File.ReadAllText(Backup));
        Assert.Equal(ProductionPreferencesLoadStatus.Loaded, Store().LoadWithResult().Status);
    }

    [Fact]
    public void UnreadablePrimaryIsNotTreatedAsMissingOrOverwritten()
    {
        Store().Save(Prefs("first"));
        using (var locked = new FileStream(Primary, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            Assert.Equal(ProductionPreferencesLoadStatus.Unreadable, Store().LoadWithResult().Status);
            Assert.Throws<IOException>(() => Store().Save(Prefs("replacement")));
        }
        Assert.Equal("first", Store().Load()?.RecordingFilenamePrefix);
    }

    [Fact]
    public void MissingPrimaryRecoversBackupAndIgnoresOrphanTemporaryFile()
    {
        Store().Save(Prefs("first"));
        Store().Save(Prefs("second"));
        File.Delete(Primary);
        File.WriteAllText(Primary + ".orphan.tmp", "{partial");
        var result = Store().LoadWithResult();
        Assert.Equal(ProductionPreferencesLoadStatus.Recovered, result.Status);
        Assert.Equal(ProductionPreferencesLoadStatus.Missing, result.PrimaryFailure);
        Assert.Equal("first", result.Preferences?.RecordingFilenamePrefix);
    }

    [Fact]
    public void SerializationFailurePreservesBothDurableFiles()
    {
        Store().Save(Prefs("first"));
        Store().Save(Prefs("second"));
        var primary = File.ReadAllText(Primary);
        var backup = File.ReadAllText(Backup);
        Assert.ThrowsAny<Exception>(() => Store().Save(new() { StreamTargetBitrateMbps = double.NaN }));
        Assert.Equal(primary, File.ReadAllText(Primary));
        Assert.Equal(backup, File.ReadAllText(Backup));
    }

    [Fact]
    public void InterruptedMigrationKeepsLoadedValuesAndEncryptedRecoverableBackup()
    {
        Directory.CreateDirectory(_folder);
        var legacy = "{\"Version\":3,\"StreamRtmpStreamKey\":\"legacy-secret\"}";
        File.WriteAllText(Primary, legacy);
        var failing = new FileProductionOutputPreferencesStore(_folder, destination =>
        {
            if (destination == Primary) throw new IOException("Interrupted migration");
        }, DpapiSecretProtector.Protect, DpapiSecretProtector.Unprotect);
        Assert.Equal("legacy-secret", failing.Load()?.StreamRtmpStreamKey);
        Assert.Equal(legacy, File.ReadAllText(Primary));
        Assert.DoesNotContain("legacy-secret", File.ReadAllText(Backup));
        File.WriteAllText(Primary, "{truncated");
        var recovery = new FileProductionOutputPreferencesStore(_folder,
            protectSecret: DpapiSecretProtector.Protect, unprotectSecret: DpapiSecretProtector.Unprotect);
        Assert.Equal("legacy-secret", recovery.Load()?.StreamRtmpStreamKey);
        Assert.DoesNotContain("legacy-secret", File.ReadAllText(Primary));
    }

    [Fact]
    public async Task ConcurrentInstancesSerializeWritesAndKeepCompleteDocuments()
    {
        Store().Save(Prefs("seed"));
        await Task.WhenAll(Enumerable.Range(0, 30).Select(i => Task.Run(() =>
        {
            Store().Save(Prefs($"show-{i}"));
            Assert.NotNull(Store().Load());
        })));
        Assert.StartsWith("show-", Store().Load()!.RecordingFilenamePrefix);
        Assert.NotNull(ProductionOutputPreferencesSerializer.Deserialize(File.ReadAllText(Backup)));
        Assert.Empty(Directory.GetFiles(_folder, "*.tmp"));
    }

    public void Dispose()
    {
        if (Directory.Exists(_folder)) Directory.Delete(_folder, recursive: true);
    }
}
