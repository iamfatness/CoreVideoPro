using System.Collections.Concurrent;

namespace CoreVideoPro.WinUI.Services;

// Keeps registration changes off the UI thread and reconciles the current
// mapping after each wait. A retired token cannot unregister a successor.
internal sealed class CaptureBridgeLifecycle(
    CaptureDeviceFrameReaderService reader,
    Func<CaptureDeviceSharedMemoryWriter.LiveMapping, Task> register,
    Func<string, Task> unregister,
    Func<TimeSpan, Task>? delay = null)
{
    private readonly ConcurrentDictionary<string, SemaphoreSlim> _registrations = new(StringComparer.Ordinal);
    private readonly Func<TimeSpan, Task> _delay = delay ?? Task.Delay;
    private int _closing;

    public bool TryPublish(CaptureDeviceFrame frame)
    {
        var result = CaptureDeviceSharedMemoryWriter.Write(
            frame.DeviceId, frame.BridgeSessionToken, frame.Bgra, frame.Width, frame.Height);
        if (result.MappingChanged)
        {
            _ = ReconcileAsync(frame.DeviceId);
        }

        // Preview can still consume a current frame when its native mapping
        // was temporarily unavailable.
        return !string.IsNullOrEmpty(result.ShmName) ||
            CaptureDeviceSharedMemoryWriter.IsCurrentSession(frame.DeviceId, frame.BridgeSessionToken);
    }

    public void ReannounceMappings()
    {
        foreach (var mapping in CaptureDeviceSharedMemoryWriter.LiveMappings())
        {
            _ = ReconcileAsync(mapping.DeviceId);
        }
    }

    public async Task StopAsync(string deviceId, BridgeCaptureStopReason reason, long? expectedToken = null)
    {
        try
        {
            var (signal, releaseDelay) = BridgeCaptureStopPolicy.For(reason);
            if (reader.StopSession(deviceId, expectedToken, signal,
                    deferDispose: reason == BridgeCaptureStopReason.Shutdown) is not { } stopped)
            {
                return;
            }

            LaunchLog.Write($"capture: bridge stopped {deviceId} signal={(uint)signal} reason={reason}");
            if (stopped.MappingToken is long token)
            {
                if (releaseDelay > TimeSpan.Zero)
                {
                    await _delay(releaseDelay).ConfigureAwait(false);
                }

                if (CaptureDeviceSharedMemoryWriter.Remove(deviceId, token))
                {
                    await ReconcileAsync(deviceId).ConfigureAwait(false);
                }
            }

            await stopped.Completion.ConfigureAwait(false);
        }
        catch (Exception error)
        {
            LaunchLog.WriteException($"capture: bridge stop {deviceId}", error);
        }
    }

    public void StopForShutdown()
    {
        Interlocked.Exchange(ref _closing, 1);
        foreach (var id in CaptureDeviceSharedMemoryWriter.TrackedDeviceIds())
        {
            _ = StopAsync(id, BridgeCaptureStopReason.Shutdown);
        }
    }

    private async Task ReconcileAsync(string deviceId)
    {
        var registration = _registrations.GetOrAdd(deviceId, static _ => new SemaphoreSlim(1, 1));
        await registration.WaitAsync().ConfigureAwait(false);
        try
        {
            if (Volatile.Read(ref _closing) != 0)
            {
                return;
            }

            if (CaptureDeviceSharedMemoryWriter.MappingFor(deviceId) is { } mapping)
            {
                await register(mapping).ConfigureAwait(false);
            }
            else
            {
                await unregister(deviceId).ConfigureAwait(false);
                LaunchLog.Write($"capture: unregister-capture-shm {deviceId}");
            }
        }
        catch (Exception error)
        {
            LaunchLog.WriteException($"capture: bridge registration {deviceId}", error);
        }
        finally
        {
            registration.Release();
        }
    }
}
