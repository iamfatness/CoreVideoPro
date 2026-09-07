using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class StudioStartupBootstrapTests
{
    [Theory]
    [InlineData(2)]
    [InlineData(3)]
    public async Task PreferenceRestoreCanLaunchFirstChildWithSavedDepthWithoutLateConfiguration(int frames)
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-bootstrap-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            var output = Path.Combine(directory, "depth.txt");
            var script = Path.Combine(directory, "child.cmd");
            await File.WriteAllTextAsync(script, $"@echo off\r\n> \"{output}\" echo %COREVIDEO_PROGRAM_BUFFER_FRAMES%\r\nexit /b 23\r\n");
            var supervisor = new MediaCoreSupervisor(new MediaCoreSupervisorOptions
            {
                Command = "cmd.exe", Args = ["/c", script], MaxRestarts = 0,
                HandshakeRequestTimeoutMs = 1500, RequestTimeoutMs = 1000
            });
            await using var bridge = new MediaCoreBridgeService(supervisor);
            var store = new Store { Preferences = new() { ProgramBufferFrames = frames } };
            var startup = StudioStartupBootstrap.Create(store, bridge);
            // Restoring other properties can synchronously start the bridge. The saved
            // buffer setting must already be installed, even if the store changes later.
            store.Preferences = new() { ProgramBufferFrames = frames == 2 ? 3 : 2 };
            Task? launch = null;
            startup.Restore(preferences =>
            {
                Assert.Equal(frames, preferences.ProgramBufferFrames);
                launch = supervisor.StartAsync();
            });
            Assert.NotNull(launch);
            // This owned test child records its environment, then deliberately exits
            // without a handshake. No application, camera, or SDK is launched.
            await Assert.ThrowsAnyAsync<InvalidOperationException>(() => launch!);
            Assert.Equal(frames.ToString(), (await File.ReadAllTextAsync(output)).Trim());
            Assert.Equal(1, store.Reads);
            Assert.Throws<InvalidOperationException>(() => bridge.ConfigureProgramBufferFrames(frames));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public async Task UnreadablePreferencesConfigureDefaultBeforeAnyRestore()
    {
        await using var bridge = new MediaCoreBridgeService();
        var startup = StudioStartupBootstrap.Create(new Store { Fail = true }, bridge);
        Assert.Equal(3, startup.ProgramBufferFrames);
        Assert.IsType<IOException>(startup.LoadError);
        Assert.Equal(ProductionPreferencesLoadStatus.Unreadable, startup.Loaded.Status);
        startup.Restore(_ => Assert.Fail("Unreadable preferences must not be restored."));
    }

    private sealed class Store : IProductionOutputPreferencesStore
    {
        public ProductionOutputPreferences? Preferences { get; set; }
        public bool Fail { get; init; }
        public int Reads { get; private set; }
        public ProductionOutputPreferences? Load()
        {
            Reads++;
            if (Fail) throw new IOException("Test preferences unavailable");
            return Preferences;
        }
        public void Save(ProductionOutputPreferences preferences) => Preferences = preferences;
    }
}
