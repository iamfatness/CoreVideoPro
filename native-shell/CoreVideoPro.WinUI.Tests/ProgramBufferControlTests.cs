using CoreVideoPro.Control;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class ProgramBufferControlTests
{
    [Fact]
    public void FailedDurableWriteRejectsApiAndRetainsSelectionThenIdenticalRetrySucceeds()
    {
        var store = new FailingStore();
        var selection = 3;
        void Save(int frames) => ProgramBufferPreferencePersistence.Save(store,
            new ProductionOutputPreferences { ProgramBufferFrames = selection, StreamRtmpStreamKey = "test-only" },
            frames, committed => selection = committed);
        var rejected = StudioControlSurface.ApplyProgramBufferRequest(2d, Save);
        Assert.False(rejected.Ok);
        Assert.Contains("not saved", rejected.Error);
        Assert.Equal(3, selection);
        Assert.Equal(3, store.Load()!.ProgramBufferFrames);
        Assert.False(ProgramBufferSettingsSummary.RequiresRestart(3, selection));
        store.Fail = false;
        Assert.True(StudioControlSurface.ApplyProgramBufferRequest(2d, Save).Ok);
        Assert.Equal(2, selection);
        Assert.Equal(2, store.Load()!.ProgramBufferFrames);
        Assert.Equal("test-only", store.Load()!.StreamRtmpStreamKey);
        Assert.True(ProgramBufferSettingsSummary.RequiresRestart(3, selection));
    }

    private sealed class FailingStore : IProductionOutputPreferencesStore
    {
        private ProductionOutputPreferences _saved = new() { ProgramBufferFrames = 3 };
        public bool Fail { get; set; } = true;
        public ProductionOutputPreferences? Load() => _saved;
        public void Save(ProductionOutputPreferences preferences)
        {
            if (Fail) throw new IOException("Simulated durable write failure");
            _saved = preferences;
        }
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2.4)]
    [InlineData(2.5)]
    [InlineData(4)]
    [InlineData(double.NaN)]
    [InlineData(double.PositiveInfinity)]
    public void WireBindingNeverRoundsInvalidDepthIntoAcceptedValue(double value)
    {
        Assert.True(ControlActionRegistry.TryBind("settings.programBuffer.set", [value], out var bound, out _));
        var called = false;
        var result = StudioControlSurface.ApplyProgramBufferRequest(bound[0], _ => called = true);
        Assert.False(result.Ok);
        Assert.False(called);
    }

    [Theory]
    [InlineData(2)]
    [InlineData(3)]
    public void AcceptedRequestPersistsAndKeepsSessionRequestUnchanged(int requested)
    {
        var directory = Path.Combine(Path.GetTempPath(), "corevideo-buffer-control-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new FileProductionOutputPreferencesStore(directory);
            var preferences = new ProductionOutputPreferences { ProgramBufferFrames = 3, StreamRtmpStreamKey = "test-only-sentinel" };
            var sessionFrames = preferences.ProgramBufferFrames;
            Assert.True(ControlActionRegistry.TryBind("settings.programBuffer.set", [requested], out var bound, out _));
            Assert.True(StudioControlSurface.ApplyProgramBufferRequest(bound[0], frames =>
            {
                preferences.ProgramBufferFrames = frames;
                store.Save(preferences);
            }).Ok);
            Assert.Equal(requested, store.Load()!.ProgramBufferFrames);
            Assert.Equal("test-only-sentinel", store.Load()!.StreamRtmpStreamKey);
            Assert.Equal(3, sessionFrames);
            Assert.Equal(requested != 3, ProgramBufferSettingsSummary.RequiresRestart(sessionFrames, preferences.ProgramBufferFrames));
        }
        finally
        {
            if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void MissingRequestIsRejectedWithoutSaving()
    {
        Assert.False(ControlActionRegistry.TryBind("settings.programBuffer.set", [], out _, out _));
        Assert.False(StudioControlSurface.ApplyProgramBufferRequest(null, _ => Assert.Fail("Must not save")).Ok);
    }
}
