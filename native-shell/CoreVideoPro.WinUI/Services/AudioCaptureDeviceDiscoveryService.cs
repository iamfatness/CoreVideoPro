using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using Microsoft.Win32;
using Windows.Devices.Enumeration;

namespace CoreVideoPro.WinUI.Services;

public sealed class AudioCaptureDeviceDiscoveryService : IDisposable
{
    private DeviceWatcher? _watcher;
    private Action? _onDevicesChanged;

    public async Task<IReadOnlyList<AudioCaptureDevice>> DiscoverDevicesAsync()
    {
        try
        {
            var deviceInfos = await DeviceInformation.FindAllAsync(DeviceClass.AudioCapture)
                .AsTask()
                .ConfigureAwait(false);

            var wasapiDevices = deviceInfos
                .Where(info => !string.IsNullOrWhiteSpace(info.Name))
                .GroupBy(info => info.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => new AudioCaptureDevice
                {
                    Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(group.Key),
                    NativeDeviceId = group.Key,
                    Name = group.First().Name,
                    SourceKind = "wasapi-input",
                    DriverName = "WASAPI"
                })
                .ToList();

            var loopbackDevices = await DiscoverWasapiLoopbackDevicesAsync().ConfigureAwait(false);

            return SortForOperatorSelection(wasapiDevices
                .Concat(loopbackDevices)
                .Concat(DiscoverAsioDevices())
                .GroupBy(device => device.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => group.First()));
        }
        catch
        {
            var loopbackDevices = await DiscoverWasapiLoopbackDevicesAsync().ConfigureAwait(false);
            return SortForOperatorSelection(loopbackDevices.Concat(DiscoverAsioDevices()));
        }
    }

    public static IReadOnlyList<AudioCaptureDevice> SortForOperatorSelection(IEnumerable<AudioCaptureDevice> devices) =>
        devices
            .OrderBy(device => SourceKindPriority(device.SourceKind))
            .ThenBy(device => device.DriverName, StringComparer.OrdinalIgnoreCase)
            .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

    public static string ResolveDefaultLocalMachineAudioDeviceId(IEnumerable<AudioCaptureDevice> devices) =>
        devices
            .Where(IsLoopbackSource)
            .OrderBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .Select(device => device.Id)
            .FirstOrDefault() ??
        devices
            .OrderBy(device => SourceKindPriority(device.SourceKind))
            .ThenBy(device => device.DriverName, StringComparer.OrdinalIgnoreCase)
            .ThenBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .Select(device => device.Id)
            .FirstOrDefault() ??
        string.Empty;

    public static bool IsLoopbackSource(AudioCaptureDevice device) =>
        device.SourceKind?.Trim().ToLowerInvariant() is "wasapi-loopback" or "loopback" or "wasapi-render-loopback" or "system-loopback";

    public static int SourceKindPriority(string? sourceKind) =>
        sourceKind?.Trim().ToLowerInvariant() switch
        {
            "wasapi-loopback" or "loopback" or "wasapi-render-loopback" or "system-loopback" => 0,
            "wasapi-input" or "wasapi-capture" => 1,
            "embedded-capture-audio" => 2,
            "asio-input" => 3,
            _ => 4
        };

    public static IReadOnlyList<AudioCaptureDevice> CreateEmbeddedCaptureAudioDevices(IEnumerable<CaptureDevice> captureDevices) =>
        captureDevices
            .Where(device =>
                device.Vendor.Equals("blackmagic", StringComparison.OrdinalIgnoreCase) ||
                device.Vendor.Equals("aja", StringComparison.OrdinalIgnoreCase))
            .Select(device => new AudioCaptureDevice
            {
                Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId($"embedded-audio:{device.Id}"),
                NativeDeviceId = device.NativeDeviceId,
                Name = $"{device.Name} embedded audio",
                SourceKind = "embedded-capture-audio",
                DriverName = device.Vendor.Equals("blackmagic", StringComparison.OrdinalIgnoreCase)
                    ? "Blackmagic DeckLink"
                    : "AJA NTV2",
                LinkedCaptureDeviceId = device.Id,
                IsAvailable = device.ConnectionState is not CaptureConnectionState.Disconnected
            })
            .OrderBy(device => device.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();

    public void StartWatching(Action onDevicesChanged)
    {
        StopWatching();
        _onDevicesChanged = onDevicesChanged;

        try
        {
            _watcher = DeviceInformation.CreateWatcher(DeviceClass.AudioCapture);
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

    private static async Task<IReadOnlyList<AudioCaptureDevice>> DiscoverWasapiLoopbackDevicesAsync()
    {
        try
        {
            var renderDevices = await DeviceInformation.FindAllAsync(DeviceClass.AudioRender)
                .AsTask()
                .ConfigureAwait(false);

            return renderDevices
                .Where(info => !string.IsNullOrWhiteSpace(info.Name))
                .GroupBy(info => info.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => new AudioCaptureDevice
                {
                    Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId($"loopback:{group.Key}"),
                    NativeDeviceId = group.Key,
                    Name = group.First().Name,
                    SourceKind = "wasapi-loopback",
                    DriverName = "WASAPI loopback"
                })
                .ToList();
        }
        catch
        {
            return [];
        }
    }

    private static IEnumerable<AudioCaptureDevice> DiscoverAsioDevices()
    {
        if (!OperatingSystem.IsWindows())
        {
            return [];
        }

        var devices = new List<AudioCaptureDevice>();
        AddAsioDevices(devices, RegistryView.Registry64);
        AddAsioDevices(devices, RegistryView.Registry32);

        return devices
            .GroupBy(device => device.NativeDeviceId, StringComparer.OrdinalIgnoreCase)
            .Select(group => group.First());
    }

    private static void AddAsioDevices(ICollection<AudioCaptureDevice> devices, RegistryView view)
    {
        try
        {
            using var localMachine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
            using var asioRoot = localMachine.OpenSubKey(@"SOFTWARE\ASIO");
            if (asioRoot is null)
            {
                return;
            }

            foreach (var driverName in asioRoot.GetSubKeyNames())
            {
                using var driverKey = asioRoot.OpenSubKey(driverName);
                if (driverKey is null)
                {
                    continue;
                }

                var clsid = driverKey.GetValue("CLSID")?.ToString();
                var description = driverKey.GetValue("Description")?.ToString();
                var nativeId = string.IsNullOrWhiteSpace(clsid)
                    ? $"asio:{driverName}"
                    : $"asio:{clsid}";

                devices.Add(new AudioCaptureDevice
                {
                    Id = CaptureDeviceDiscoveryMapper.CreateStableDeviceId(nativeId),
                    NativeDeviceId = nativeId,
                    Name = string.IsNullOrWhiteSpace(description) ? driverName : description,
                    SourceKind = "asio-input",
                    DriverName = "ASIO"
                });
            }
        }
        catch
        {
            // ASIO discovery is opportunistic. Missing registry access should not hide WASAPI devices.
        }
    }
}
