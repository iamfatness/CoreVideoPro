using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Windows.Devices.Enumeration;

namespace CoreVideoPro.WinUI.Services;

public sealed class CaptureDeviceDiscoveryService : IDisposable
{
    private DeviceWatcher? _watcher;
    private Action? _onDevicesChanged;

    public async Task<IReadOnlyList<CaptureDevice>> DiscoverDevicesAsync()
    {
        try
        {
            var deviceInfos = await DeviceInformation.FindAllAsync(DeviceClass.VideoCapture)
                .AsTask()
                .ConfigureAwait(false);

            var devices = deviceInfos
                .Where(info => !string.IsNullOrWhiteSpace(info.Name))
                .GroupBy(info => info.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => MapDiscoveredDevice(group.Key, group.First().Name));

            return SortForOperatorSelection(devices);
        }
        catch
        {
            return [];
        }
    }

    public IReadOnlyList<CaptureDevice> DiscoverDevices() =>
        DiscoverDevicesAsync().GetAwaiter().GetResult();

    public void StartWatching(Action onDevicesChanged)
    {
        StopWatching();
        _onDevicesChanged = onDevicesChanged;

        try
        {
            _watcher = DeviceInformation.CreateWatcher(DeviceClass.VideoCapture);
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

    public static IReadOnlyList<CaptureDevice> SortForOperatorSelection(IEnumerable<CaptureDevice> devices) =>
        devices
            .OrderBy(device => CaptureDeviceDiscoveryMapper.OperatorSelectionPriority(device.Name, device.Vendor))
            .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

    internal static CaptureDevice MapDiscoveredDevice(string deviceSymbolicLinkId, string friendlyName) =>
        new()
        {
            Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(deviceSymbolicLinkId),
            NativeDeviceId = deviceSymbolicLinkId,
            Vendor = CaptureDeviceDiscoveryMapper.DetectVendor(friendlyName),
            Name = friendlyName,
            Inputs = [new CaptureDeviceInput { Id = "default", Label = "Default" }],
            SelectedInputId = "default",
            Width = 0,
            Height = 0,
            FrameRate = 0,
            ConnectionState = CaptureConnectionState.Detected,
            SignalPresent = false,
            AudioSyncOffsetMs = 0
        };
}
