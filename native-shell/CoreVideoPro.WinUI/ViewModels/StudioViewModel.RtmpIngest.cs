using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels.ShowInputs;

namespace CoreVideoPro.WinUI.ViewModels;

public sealed partial class StudioViewModel
{
    private readonly RtmpIngestSourcesCoordinator _rtmpIngestCoordinator;

    public ObservableCollection<RtmpIngestSource> RtmpIngestSources => _rtmpIngestCoordinator.Sources;
    public bool CanAddRtmpIngestSource => _rtmpIngestCoordinator.CanAdd;
    public string RtmpIngestCountSummary => $"{RtmpIngestSources.Count} of {RtmpIngestSourcesCoordinator.MaxSources} RTMP sources";
    public string RtmpIngestRuntimeSummary =>
        "Publish to the listener URL, then select its RTMP source in Show inputs. Each source needs a unique port.";

    [RelayCommand(CanExecute = nameof(CanAddRtmpIngestSource))]
    private void AddRtmpIngestSource()
    {
        var added = _rtmpIngestCoordinator.Add();
        CommandStatus = added is null
            ? $"RTMP ingest is capped at {RtmpIngestSourcesCoordinator.MaxSources} sources."
            : _rtmpIngestCoordinator.PersistenceError ? "RTMP settings could not be saved." :
                $"{added.Name} added. Enter its listener URL.";
    }

    [RelayCommand]
    private void RemoveRtmpIngestSource(string sourceId)
    {
        var removed = _rtmpIngestCoordinator.Remove(sourceId);
        CommandStatus = removed is null
            ? "Keep at least one RTMP ingest source configured."
            : _rtmpIngestCoordinator.PersistenceError ? "RTMP settings could not be saved." :
                $"{removed.Name} removed from RTMP inputs.";
    }

    private static IRtmpIngestSourceStore CreateRtmpIngestSourceStore()
    {
        string folder;
        try { folder = Windows.Storage.ApplicationData.Current.LocalFolder.Path; }
        catch (Exception)
        {
            folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "CoreVideoPro");
        }
        return new FileRtmpIngestSourceStore(folder, DpapiSecretProtector.Protect, DpapiSecretProtector.Unprotect);
    }

    private async Task<bool> TryConnectRtmpIngestAsync(CaptureDevice device)
    {
        if (!device.Vendor.Equals("rtmp", StringComparison.OrdinalIgnoreCase)) return false;
        if (!_rtmpIngestCoordinator.BuildWire().Any(source => source.DeviceId == device.Id))
        {
            CommandStatus = "Enter a valid, unique RTMP listener URL before connecting this source.";
            return true;
        }
        try
        {
            await EnsureMediaCoreRunningAsync("Preparing RTMP ingest...").ConfigureAwait(false);
            if (_bridge.Profile?.HasAvailableCapability("rtmp-ingest") != true)
            {
                RunOnUiThread(() => CommandStatus = "RTMP ingest is unavailable in this native build.");
                return true;
            }
            await TrySyncMediaCoreAsync().ConfigureAwait(false);
            var statuses = await _bridge.ConnectNativeCaptureDeviceAsync(device.Id).ConfigureAwait(false);
            var match = statuses.FirstOrDefault(status => status.Id == device.Id);
            RunOnUiThread(() =>
            {
                device.ConnectionState = match is null || match.ConnectionState is "error" or "failed"
                    ? CaptureConnectionState.Error : CaptureConnectionState.Connected;
                device.SignalPresent = match?.SignalPresent ?? false;
                if (device.ConnectionState == CaptureConnectionState.Connected)
                    AssignConnectedCaptureDeviceToShowInput(device);
                RefreshCaptureFleetSummary();
                RefreshShowInputEditors();
                RefreshPreviewRoutingState();
                RefreshMultiviewGridTiles();
                CommandStatus = match is null
                    ? "RTMP ingest is unavailable in this native build."
                    : device.ConnectionState == CaptureConnectionState.Error
                        ? "RTMP listener failed to start. Check the port and native diagnostics."
                        : "RTMP listener ready. Waiting for publisher video and audio.";
            });
        }
        catch (Exception error)
        {
            LaunchLog.Write($"rtmp ingest: connect failed: {error.GetType().Name}");
            RunOnUiThread(() => CommandStatus = "RTMP listener failed to start. Check the native build and port.");
        }
        return true;
    }

    private void OnRtmpIngestSourcesChanged()
    {
        if (_rtmpIngestCoordinator.PersistenceError) CommandStatus = "RTMP settings could not be saved.";
        OnPropertyChanged(nameof(RtmpIngestCountSummary));
        OnPropertyChanged(nameof(CanAddRtmpIngestSource));
        AddRtmpIngestSourceCommand.NotifyCanExecuteChanged();
        RefreshVirtualRtmpIngestDevices();
        RefreshShowInputEditors();
        RefreshPreviewRoutingState();
        RefreshMultiviewGridTiles();
        _ = SyncRtmpIngestSourcesAsync(_rtmpIngestCoordinator.BuildWire().Select(source => source.DeviceId).ToList());
    }

    private async Task SyncRtmpIngestSourcesAsync(IReadOnlyList<string> deviceIds)
    {
        try
        {
            await TrySyncMediaCoreAsync().ConfigureAwait(false);
            if (_bridge.Profile?.HasAvailableCapability("rtmp-ingest") == true)
            {
                foreach (var deviceId in deviceIds)
                {
                    await _bridge.ConnectNativeCaptureDeviceAsync(deviceId).ConfigureAwait(false);
                }
            }
            await RefreshCaptureDevicesAsync().ConfigureAwait(false);
        }
        catch (Exception error)
        {
            LaunchLog.Write($"rtmp ingest: source sync failed: {error.GetType().Name}");
        }
    }

    private void RefreshVirtualRtmpIngestDevices()
    {
        foreach (var source in RtmpIngestSources)
        {
            var next = CreateVirtualRtmpIngestDevice(source);
            var prior = CaptureDevices.FirstOrDefault(device => device.Id == source.DeviceId);
            if (prior is null)
            {
                ProductionStateHelper.PopulateCaptureDeviceDropoutPolicy(next, _sourceDropoutPolicies);
                CaptureDevices.Add(next);
            }
            else
            {
                next.ConnectionState = RtmpIngestSourcePolicy.IsValid(source.Url)
                    ? prior.ConnectionState : CaptureConnectionState.Detected;
                next.SignalPresent = RtmpIngestSourcePolicy.IsValid(source.Url) && prior.SignalPresent;
                next.SelectedInputId = prior.SelectedInputId;
                next.AudioSyncOffsetMs = prior.AudioSyncOffsetMs;
                next.ObservedFrameWidth = prior.ObservedFrameWidth;
                next.ObservedFrameHeight = prior.ObservedFrameHeight;
                next.ObservedFrameRate = prior.ObservedFrameRate;
                next.DropoutPolicy = prior.DropoutPolicy;
                ProductionStateHelper.PopulateCaptureDeviceDropoutPolicy(next, _sourceDropoutPolicies);
                CaptureDevices[CaptureDevices.IndexOf(prior)] = next;
            }
        }

        var active = RtmpIngestSources.Select(source => source.DeviceId).ToHashSet(StringComparer.Ordinal);
        foreach (var stale in CaptureDevices.Where(device => device.Vendor == "rtmp" && !active.Contains(device.Id)).ToList())
        {
            RemoveVirtualNetworkIngestDevice(stale.Id);
        }
        RefreshCaptureFleetSummary();
        OnPropertyChanged(nameof(HasCaptureDevices));
    }

    private void ApplyRtmpIngestSnapshot(NativeMediaCoreStateSnapshot snapshot)
    {
        if (snapshot.CaptureDevices is null) return;
        var changed = false;
        var facts = RtmpIngestCaptureFacts.Read(snapshot.CaptureDevices);
        foreach (var fact in facts)
        {
            var device = CaptureDevices.FirstOrDefault(item => item.Id == fact.DeviceId);
            if (device is not null) changed |= RtmpIngestCaptureFacts.Apply(device, fact);
        }
        var active = facts.Select(fact => fact.DeviceId).ToHashSet(StringComparer.Ordinal);
        foreach (var device in CaptureDevices.Where(item => item.Vendor == "rtmp" && !active.Contains(item.Id)))
        {
            changed |= RtmpIngestCaptureFacts.Apply(device,
                new RtmpIngestCaptureFacts.Fact(device.Id, "connecting", false, 0, 0, 0));
        }
        if (changed)
        {
            RefreshCaptureFleetSummary();
            RefreshShowInputEditors();
            RefreshMultiviewGridTiles();
        }
    }

    private IReadOnlyList<CaptureDevice> CreateVirtualRtmpIngestDevices() =>
        RtmpIngestSources.Select(CreateVirtualRtmpIngestDevice).ToList();

    private static CaptureDevice CreateVirtualRtmpIngestDevice(RtmpIngestSource source) => new()
    {
        Id = source.DeviceId,
        NativeDeviceId = source.DeviceId,
        Vendor = "rtmp",
        Name = source.Name,
        Inputs = [new CaptureDeviceInput { Id = source.Id, Label = source.Name }],
        SelectedInputId = source.Id,
        Width = 1920,
        Height = 1080,
        FrameRate = 60,
        ConnectionState = CaptureConnectionState.Detected,
        SignalPresent = false,
        AudioSyncOffsetMs = 0
    };
}
