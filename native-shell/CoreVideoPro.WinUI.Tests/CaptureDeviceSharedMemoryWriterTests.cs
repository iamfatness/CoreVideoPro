using System;
using System.IO.MemoryMappedFiles;
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
        var token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);

        var first = CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        Assert.True(first.MappingChanged, "the first frame must announce a new buffer");

        var second = CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        Assert.False(second.MappingChanged);

        // This is the whole hazard in one assertion: a steady stream of frames NEVER
        // re-announces, so nothing here can tell a fresh core the buffer exists.
        var third = CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        Assert.False(third.MappingChanged);
    }

    [Fact]
    public void LiveMappings_ExposesEveryBufferSoAFreshCoreCanBeReAnnounced()
    {
        var deviceA = DeviceId();
        var deviceB = DeviceId();
        var tokenA = CaptureDeviceSharedMemoryWriter.BeginSession(deviceA);
        var tokenB = CaptureDeviceSharedMemoryWriter.BeginSession(deviceB);

        var a = CaptureDeviceSharedMemoryWriter.Write(deviceA, tokenA, Frame(64, 36), 64, 36);
        var b = CaptureDeviceSharedMemoryWriter.Write(deviceB, tokenB, Frame(32, 18), 32, 18);

        // Steady-state frames — the state a core respawn actually interrupts.
        CaptureDeviceSharedMemoryWriter.Write(deviceA, tokenA, Frame(64, 36), 64, 36);
        CaptureDeviceSharedMemoryWriter.Write(deviceB, tokenB, Frame(32, 18), 32, 18);

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
        var token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);

        CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        var resized = CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(96, 54), 96, 54);
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
        var token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        Assert.Contains(CaptureDeviceSharedMemoryWriter.LiveMappings(), mapping => mapping.DeviceId == deviceId);

        Assert.True(CaptureDeviceSharedMemoryWriter.Remove(deviceId, token), "a buffer existed, so the caller must unregister it with the core");

        Assert.DoesNotContain(CaptureDeviceSharedMemoryWriter.LiveMappings(), mapping => mapping.DeviceId == deviceId);
        Assert.False(CaptureDeviceSharedMemoryWriter.Remove(deviceId, token), "nothing left to unregister");
    }

    [Fact]
    public void Write_AfterRemove_AnnouncesAFreshBuffer()
    {
        var deviceId = DeviceId();
        var token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        CaptureDeviceSharedMemoryWriter.Retire(deviceId, token, CaptureBridgeSignal.Stopped);
        CaptureDeviceSharedMemoryWriter.Remove(deviceId, token);
        token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);

        // A later bridge session for the same device must register again, or the core
        // (which was told to let go) would never see its frames.
        var restarted = CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36);
        Assert.True(restarted.MappingChanged);
        CaptureDeviceSharedMemoryWriter.Remove(deviceId, token);
    }

    [Fact]
    public void WritePublishesLiveAndRetirementChangesOnlyTheSignalWord()
    {
        var deviceId = DeviceId();
        var token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        try
        {
            var pixels = Frame(64, 36);
            pixels[0] = 91;
            var write = CaptureDeviceSharedMemoryWriter.Write(deviceId, token, pixels, 64, 36);
            using var file = MemoryMappedFile.OpenExisting(write.ShmName, MemoryMappedFileRights.Read);
            using var view = file.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
            var sequence = view.ReadUInt32(0);
            Assert.Equal((uint)CaptureBridgeSignal.Live, view.ReadUInt32(12));
            Assert.Equal(token, CaptureDeviceSharedMemoryWriter.Retire(deviceId, token, CaptureBridgeSignal.Lost)?.SessionToken);
            Assert.Equal((uint)CaptureBridgeSignal.Lost, view.ReadUInt32(12));
            Assert.Equal(sequence, view.ReadUInt32(0));
            Assert.Equal((byte)91, view.ReadByte(16));
            Assert.Null(CaptureDeviceSharedMemoryWriter.Write(deviceId, token, Frame(64, 36), 64, 36).ShmName);
        }
        finally
        {
            CaptureDeviceSharedMemoryWriter.Remove(deviceId, token);
        }
    }

    [Fact]
    public void OldSessionCannotWriteOrRemoveTheReplacementMapping()
    {
        var deviceId = DeviceId();
        var first = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        var prior = CaptureDeviceSharedMemoryWriter.Write(deviceId, first, Frame(64, 36), 64, 36);
        CaptureDeviceSharedMemoryWriter.Retire(deviceId, first, CaptureBridgeSignal.Lost);
        var replacement = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        try
        {
            var next = CaptureDeviceSharedMemoryWriter.Write(deviceId, replacement, Frame(64, 36), 64, 36);
            Assert.NotEqual(prior.ShmName, next.ShmName);
            Assert.Null(CaptureDeviceSharedMemoryWriter.Write(deviceId, first, Frame(64, 36), 64, 36).ShmName);
            Assert.False(CaptureDeviceSharedMemoryWriter.Remove(deviceId, first));
            Assert.Equal(next.ShmName, CaptureDeviceSharedMemoryWriter.MappingFor(deviceId)?.ShmName);
        }
        finally
        {
            CaptureDeviceSharedMemoryWriter.Retire(deviceId, replacement, CaptureBridgeSignal.Stopped);
            CaptureDeviceSharedMemoryWriter.Remove(deviceId, replacement);
        }
    }

    [Fact]
    public async Task DeliberateStopRemovesTheMappingAndUnregistersIt()
    {
        var deviceId = DeviceId();
        var token = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        var events = new List<string>();
        using var reader = new CaptureDeviceFrameReaderService();
        var bridge = new CaptureBridgeLifecycle(reader,
            mapping => { events.Add("register:" + mapping.DeviceId); return Task.CompletedTask; },
            id => { events.Add("unregister:" + id); return Task.CompletedTask; });
        Assert.True(bridge.TryPublish(BridgeFrame(deviceId, token)));

        await bridge.StopAsync(deviceId, BridgeCaptureStopReason.NativeTakeover);

        Assert.Equal(new[] { "register:" + deviceId, "unregister:" + deviceId }, events);
        Assert.Null(CaptureDeviceSharedMemoryWriter.MappingFor(deviceId));
        Assert.False(bridge.TryPublish(BridgeFrame(deviceId, token)));
    }

    [Fact]
    public async Task DelayedLossCannotUnregisterAReplacementSession()
    {
        var deviceId = DeviceId();
        var first = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var events = new List<string>();
        using var reader = new CaptureDeviceFrameReaderService();
        var bridge = new CaptureBridgeLifecycle(reader,
            mapping => { events.Add(mapping.ShmName); return Task.CompletedTask; },
            id => { events.Add("unregister:" + id); return Task.CompletedTask; },
            delay: _ => release.Task);
        try
        {
            Assert.True(bridge.TryPublish(BridgeFrame(deviceId, first)));
            var lost = bridge.StopAsync(deviceId, BridgeCaptureStopReason.ReaderLost, first);
            Assert.False(lost.IsCompleted);
            Assert.False(bridge.TryPublish(BridgeFrame(deviceId, first)));

            var replacement = CaptureDeviceSharedMemoryWriter.BeginSession(deviceId);
            Assert.True(bridge.TryPublish(BridgeFrame(deviceId, replacement)));
            release.TrySetResult();
            await lost;

            Assert.Equal(2, events.Count);
            Assert.NotEqual(events[0], events[1]);
            Assert.Equal(events[1], CaptureDeviceSharedMemoryWriter.MappingFor(deviceId)?.ShmName);
        }
        finally
        {
            release.TrySetResult();
            await bridge.StopAsync(deviceId, BridgeCaptureStopReason.OperatorDisconnect);
        }
    }

    private static CaptureDeviceFrame BridgeFrame(string deviceId, long token) => new()
    {
        DeviceId = deviceId,
        BridgeSessionToken = token,
        Bgra = Frame(64, 36),
        Width = 64,
        Height = 36,
        FrameId = 1
    };
}
