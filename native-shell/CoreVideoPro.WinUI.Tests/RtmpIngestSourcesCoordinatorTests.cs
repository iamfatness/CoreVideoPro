using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels.ShowInputs;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class RtmpIngestSourcesCoordinatorTests
{
    [Theory]
    [InlineData("rtmps://127.0.0.1/live/stream")]
    [InlineData("rtmp://user:secret@127.0.0.1/live/stream")]
    [InlineData("rtmp://127.0.0.1/live/stream?token=secret")]
    [InlineData("rtmp://127.0.0.1/live")]
    [InlineData("rtmp://127.0.0.1/live/white space")]
    [InlineData("rtmp://127.0.0.1:0/live/test")]
    public void RefusesUnsupportedListenerUrl(string url) =>
        Assert.False(RtmpIngestSourcePolicy.IsValid(url));

    [Fact]
    public void EditingListenerPersistsAndProjectsNativeSourceWithoutLeakingStreamPath()
    {
        var store = new InMemoryRtmpIngestSourceStore();
        using (var coordinator = new RtmpIngestSourcesCoordinator(store))
        {
            Assert.Empty(coordinator.BuildWire());
            var source = Assert.Single(coordinator.Sources);
            source.Url = "rtmp://0.0.0.0:1935/live/secret-stream";
            var wire = Assert.Single(coordinator.BuildWire());
            Assert.Equal("rtmp-ingest-01", wire.DeviceId);
            Assert.Equal(source.Url, wire.Url);
            Assert.DoesNotContain("secret-stream", source.Name + source.Summary);
            source.Url = "rtmps://example.com/live/secret-stream";
            Assert.Empty(coordinator.BuildWire());
            source.Url = "rtmp://0.0.0.0:1935/live/secret-stream";
        }
        using var restored = new RtmpIngestSourcesCoordinator(store);
        Assert.Equal("rtmp://0.0.0.0:1935/live/secret-stream", Assert.Single(restored.BuildWire()).Url);
    }

    [Fact]
    public void SourcesKeepStableDeviceIdsAcrossRemoveAndAdd()
    {
        using var coordinator = new RtmpIngestSourcesCoordinator(new InMemoryRtmpIngestSourceStore());
        var second = coordinator.Add()!;
        Assert.Equal("rtmp-ingest-02", second.DeviceId);
        Assert.NotNull(coordinator.Remove(second.Id));
        Assert.Equal("rtmp-ingest-02", coordinator.Add()!.DeviceId);
        var device = new CaptureDevice
        {
            Id = "rtmp-ingest-01", NativeDeviceId = "rtmp-ingest-01",
            Vendor = "rtmp", Name = "RTMP 1", Inputs = [], SelectedInputId = "rtmp-source-01"
        };
        Assert.Equal(ShowInputKind.RtmpIngest, ShowInputRosterService.InferCaptureDeviceKind(device));
        Assert.Equal("capture:rtmp-ingest-01", ShowInputRosterService.CaptureSourceId(device.Id));
        var option = Assert.Single(ShowInputRosterService.BuildUnifiedSourceOptions([], [device], [])
            .Where(item => item.Value == "capture:rtmp-ingest-01"));
        Assert.Equal("RTMP", option.Group);
        var slot = new ShowInputSlot { SlotNumber = 1, Kind = ShowInputKind.RtmpIngest,
            CaptureDeviceId = device.Id, InShow = true };
        Assert.True(slot.IsAssigned);
        Assert.Equal("capture:rtmp-ingest-01", ShowInputRosterService.SlotSourceId(slot));
    }

    [Fact]
    public void SavedUrlIsProtectedOnDisk()
    {
        var folder = Path.Combine(Path.GetTempPath(), "corevideo-rtmp-store-" + Guid.NewGuid().ToString("N"));
        try
        {
            var store = new FileRtmpIngestSourceStore(folder,
                plain => "protected:" + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(plain)),
                stored => System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(stored["protected:".Length..])));
            store.Save([new RtmpIngestSourceRecord(1, "rtmp://127.0.0.1:1935/live/secret")]);
            Assert.DoesNotContain("secret", File.ReadAllText(Path.Combine(folder, FileRtmpIngestSourceStore.FileName)));
            Assert.Equal("rtmp://127.0.0.1:1935/live/secret", Assert.Single(store.Load()).Url);
        }
        finally
        {
            if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        }
    }

    [Fact]
    public void TwoListenersOnSamePortCannotBothStart()
    {
        using var coordinator = new RtmpIngestSourcesCoordinator(new InMemoryRtmpIngestSourceStore());
        coordinator.Sources[0].Url = "rtmp://0.0.0.0:1935/live/a";
        coordinator.Add()!.Url = "rtmp://127.0.0.1:1935/other/b";
        Assert.Single(coordinator.BuildWire());
        coordinator.Sources[1].Url = "rtmp://0.0.0.0:1936/live/b";
        Assert.Equal(2, coordinator.BuildWire().Count);
    }

    [Fact]
    public void PersistenceFailureIsReportedWithoutDroppingLiveOperatorEdit()
    {
        using var coordinator = new RtmpIngestSourcesCoordinator(new FailingStore());
        coordinator.Sources[0].Url = "rtmp://127.0.0.1:1935/live/test";
        Assert.True(coordinator.PersistenceError);
        Assert.Single(coordinator.BuildWire());
    }

    private sealed class FailingStore : IRtmpIngestSourceStore
    {
        public IReadOnlyList<RtmpIngestSourceRecord> Load() => [];
        public void Save(IReadOnlyList<RtmpIngestSourceRecord> sources) => throw new IOException("disk unavailable");
    }
}
