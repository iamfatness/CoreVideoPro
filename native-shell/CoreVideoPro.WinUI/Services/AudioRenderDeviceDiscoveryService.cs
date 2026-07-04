using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Windows.Devices.Enumeration;
using Windows.Media.Devices;

namespace CoreVideoPro.WinUI.Services;

public sealed class AudioRenderDeviceDiscoveryService : IDisposable
{
    // Stable id for the "System default output" pseudo-device. Its
    // NativeDeviceId is EMPTY, which the core's WASAPI monitor adapter resolves
    // to the current OS default render endpoint — so the selection keeps
    // following the default when the user changes it in Windows. First-run
    // selection lands here instead of on an arbitrary alphabetical device
    // (audio tab redesign phase B2).
    public const string SystemDefaultDeviceId = "default-render-output";

    private DeviceWatcher? _watcher;
    private Action? _onDevicesChanged;

    public async Task<IReadOnlyList<AudioRenderDevice>> DiscoverDevicesAsync()
    {
        try
        {
            var deviceInfos = await DeviceInformation.FindAllAsync(DeviceClass.AudioRender)
                .AsTask()
                .ConfigureAwait(false);

            var defaultRenderId = NormalizeDeviceId(TryGetDefaultRenderId());
            var devices = deviceInfos
                .Where(info => !string.IsNullOrWhiteSpace(info.Name))
                .GroupBy(info => info.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => new AudioRenderDevice
                {
                    Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(group.Key),
                    NativeDeviceId = group.Key,
                    Name = group.First().Name,
                    IsDefault = !string.IsNullOrWhiteSpace(defaultRenderId) &&
                        NormalizeDeviceId(group.Key) == defaultRenderId
                })
                .OrderBy(device => device.IsDefault ? 0 : 1)
                .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
                .ToList();

            devices.Insert(0, new AudioRenderDevice
            {
                Id = SystemDefaultDeviceId,
                NativeDeviceId = string.Empty,
                Name = "System default output",
                IsDefault = true
            });
            return devices;
        }
        catch
        {
            return [];
        }
    }

    private static string? TryGetDefaultRenderId()
    {
        try
        {
            return MediaDevice.GetDefaultAudioRenderId(AudioDeviceRole.Default);
        }
        catch
        {
            return null;
        }
    }

    private static string NormalizeDeviceId(string? value) =>
        string.IsNullOrWhiteSpace(value) ? string.Empty : value.Trim().ToUpperInvariant();

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
