using System;
using System.Linq;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Regression cover for the capture half of the "multiviewer is broken" report
/// (2026-08-08): after the supervisor respawns the core under a LIVE shell, the
/// shell-bridged capture cameras never re-delivered pixels and sat on the
/// placeholder tile forever.
///
/// Cause: <see cref="CaptureDeviceSharedMemoryWriter.Write"/> reports
/// MappingChanged only when it CREATES a buffer, and that is the only thing that
/// triggers register-capture-shm. A core respawn does not touch the shell's
/// mapping, so the registration was never re-sent and the fresh core had no idea
/// those sources existed — while the bridge went on writing frames at full rate
/// into shared memory nobody was reading.
/// </summary>
public class CaptureDeviceSharedMemoryWriterTests
{
    private static byte[] Frame(int width, int height) => new byte[width * height * 4];

    // Unique per test run so parallel/repeat runs never collide on a mapping name.
    private static string DeviceId([System.Runtime.CompilerServices.CallerMemberName] string caller = "") =>
        $"cvp-test-{caller}-{Guid.NewGuid():N}";

    [Fact]
    public void Write_AnnouncesOnlyWhenTheMappingIsCreated()
    {
        var deviceId = DeviceId();

        var first = CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        Assert.True(first.MappingChanged, "the first frame must announce a new buffer");

        var second = CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        Assert.False(second.MappingChanged);

        // This is the whole hazard in one assertion: a steady stream of frames NEVER
        // re-announces, so nothing here can tell a fresh core the buffer exists.
        var third = CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        Assert.False(third.MappingChanged);
    }

    [Fact]
    public void LiveMappings_ExposesEveryBufferSoAFreshCoreCanBeReAnnounced()
    {
        var deviceA = DeviceId();
        var deviceB = DeviceId();

        var a = CaptureDeviceSharedMemoryWriter.Write(deviceA, Frame(64, 36), 64, 36);
        var b = CaptureDeviceSharedMemoryWriter.Write(deviceB, Frame(32, 18), 32, 18);

        // Steady-state frames — the state a core respawn actually interrupts.
        CaptureDeviceSharedMemoryWriter.Write(deviceA, Frame(64, 36), 64, 36);
        CaptureDeviceSharedMemoryWriter.Write(deviceB, Frame(32, 18), 32, 18);

        var live = CaptureDeviceSharedMemoryWriter.LiveMappings();

        var liveA = Assert.Single(live.Where(m => m.DeviceId == deviceA));
        Assert.Equal(a.ShmName, liveA.ShmName);
        Assert.Equal(64, liveA.Width);
        Assert.Equal(36, liveA.Height);

        var liveB = Assert.Single(live.Where(m => m.DeviceId == deviceB));
        Assert.Equal(b.ShmName, liveB.ShmName);
        Assert.Equal(32, liveB.Width);
        Assert.Equal(18, liveB.Height);
    }

    [Fact]
    public void LiveMappings_TracksAResolutionChange()
    {
        var deviceId = DeviceId();

        CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        var resized = CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(96, 54), 96, 54);
        Assert.True(resized.MappingChanged, "a resolution change re-creates the buffer");

        // The re-announcement must carry the CURRENT name and dims — replaying a stale
        // name would point the fresh core at a buffer that no longer exists.
        var live = Assert.Single(CaptureDeviceSharedMemoryWriter.LiveMappings()
            .Where(m => m.DeviceId == deviceId));
        Assert.Equal(resized.ShmName, live.ShmName);
        Assert.Equal(96, live.Width);
        Assert.Equal(54, live.Height);
    }

    // #758: a stopped bridge session must take its buffer with it. The core re-emits a
    // registered buffer's last frame forever and lets it override a live native frame for
    // the same capture id, and a respawned core is re-told about every mapping that still
    // exists.
    [Fact]
    public void Remove_DropsTheBufferSoItIsNeverReAnnounced()
    {
        var deviceId = DeviceId();
        CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        Assert.Contains(CaptureDeviceSharedMemoryWriter.LiveMappings(), mapping => mapping.DeviceId == deviceId);

        Assert.True(CaptureDeviceSharedMemoryWriter.Remove(deviceId), "a buffer existed, so the caller must unregister it with the core");

        Assert.DoesNotContain(CaptureDeviceSharedMemoryWriter.LiveMappings(), mapping => mapping.DeviceId == deviceId);
        Assert.False(CaptureDeviceSharedMemoryWriter.Remove(deviceId), "nothing left to unregister");
    }

    [Fact]
    public void Write_AfterRemove_AnnouncesAFreshBuffer()
    {
        var deviceId = DeviceId();
        CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        CaptureDeviceSharedMemoryWriter.Remove(deviceId);

        // A later bridge session for the same device must register again, or the core
        // (which was told to let go) would never see its frames.
        var restarted = CaptureDeviceSharedMemoryWriter.Write(deviceId, Frame(64, 36), 64, 36);
        Assert.True(restarted.MappingChanged);
        CaptureDeviceSharedMemoryWriter.Remove(deviceId);
    }

    [Fact]
    public void TheReaderStopPathReleasesTheBridgeBuffer()
    {
        var code = ReadViewModel("StudioViewModel.cs");
        var stop = code.IndexOf("await _captureFrameReader.StopAsync(device.Id)", StringComparison.Ordinal);
        Assert.True(stop >= 0, "the bridge reader stop call moved; update this test");
        var after = code.Substring(stop, 900);
        Assert.Contains("ReleaseCaptureBridgeBuffer(device.Id);", after, StringComparison.Ordinal);
        Assert.Contains("_bridge.UnregisterCaptureShmAsync(deviceId)", code, StringComparison.Ordinal);
    }

    private static string ReadViewModel(string fileName)
    {
        for (var directory = new System.IO.DirectoryInfo(AppContext.BaseDirectory);
             directory is not null;
             directory = directory.Parent)
        {
            var candidate = System.IO.Path.Combine(
                directory.FullName, "native-shell", "CoreVideoPro.WinUI", "ViewModels", fileName);
            if (System.IO.File.Exists(candidate))
            {
                return System.IO.File.ReadAllText(candidate);
            }
        }

        throw new System.IO.FileNotFoundException(fileName);
    }
}
