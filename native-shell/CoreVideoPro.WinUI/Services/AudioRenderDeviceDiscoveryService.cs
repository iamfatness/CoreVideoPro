using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Windows.Devices.Enumeration;

namespace CoreVideoPro.WinUI.Services;

public sealed class AudioRenderDeviceDiscoveryService : IDisposable
{
    private DeviceWatcher? _watcher;
    private Action? _onDevicesChanged;

    public async Task<IReadOnlyList<AudioRenderDevice>> DiscoverDevicesAsync()
    {
        try
        {
            var deviceInfos = await DeviceInformation.FindAllAsync(DeviceClass.AudioRender)
                .AsTask()
                .ConfigureAwait(false);

            return deviceInfos
                .Where(info => !string.IsNullOrWhiteSpace(info.Name))
                .GroupBy(info => info.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => new AudioRenderDevice
                {
                    Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(group.Key),
                    NativeDeviceId = group.Key,
                    Name = group.First().Name
                })
                .OrderBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
                .ToList();
        }
        catch
        {
            return [];
        }
    }

    public void StartWatching(Action onDevicesChanged)
    {
        StopWatching();
        _onDevicesChanged = onDevicesChanged;

        try
        {
            _watcher = DeviceInformation.CreateWatcher(DeviceClass.AudioRender);
            _watcher.Added += OnWatcherChanged;
            _watcher.Removed += OnWatcherChanged;
            _watcher.Updated += OnWatcherChanged;
            _watcher.EnumerationCompleted += OnWatcherChanged;
            _watcher.Start();
        }
        catch
        {
            StopWatching();
        }
    }

    public void StopWatching()
    {
        if (_watcher is null)
        {
            return;
        }

        _watcher.Added -= OnWatcherChanged;
        _watcher.Removed -= OnWatcherChanged;
        _watcher.Updated -= OnWatcherChanged;
        _watcher.EnumerationCompleted -= OnWatcherChanged;
        _watcher.Stop();
        _watcher = null;
    }

    public void Dispose()
    {
        StopWatching();
        _onDevicesChanged = null;
    }

    private void OnWatcherChanged(DeviceWatcher sender, object args) =>
        _onDevicesChanged?.Invoke();
}
