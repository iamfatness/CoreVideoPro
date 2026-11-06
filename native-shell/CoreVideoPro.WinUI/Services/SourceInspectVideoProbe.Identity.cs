using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

internal sealed partial class SourceInspectVideoProbe
{
    // QA-only producer. Production capture and thumbnails retain GPU delivery.
    private sealed class IdentitySource(int channel) : IAsyncDisposable
    {
        internal const int Width = 1920, Height = 1080;
        internal string Id { get; } = "identity-" + channel;
        internal string MappingName { get; } = "Local\\CoreVideo-Identity-" + Guid.NewGuid().ToString("N");
        private readonly CancellationTokenSource _stop = new();
        private MemoryMappedFile? _mapping;
        private Task? _writer;
        internal void Start()
        {
            _mapping = MemoryMappedFile.CreateNew(MappingName, 16 + Width * Height * 4);
            var mapping = _mapping;
            _writer = Task.Run(async () =>
            {
                using var view = mapping.CreateViewAccessor();
                var pixels = new byte[Width * Height * 4];
                view.Write(4, Width); view.Write(8, Height);
                uint sequence = 0;
                while (!_stop.IsCancellationRequested)
                {
                    var intensity = (byte)(160 + (sequence / 2) % 80);
                    for (var i = 0; i < pixels.Length; i += 4)
                    { pixels[i + channel] = intensity; pixels[i + 3] = 255; }
                    sequence += 2;
                    view.Write(0, sequence - 1);
                    view.WriteArray(16, pixels, 0, pixels.Length);
                    view.Write(0, sequence);
                    await Task.Delay(33);
                }
            });
        }
        public async ValueTask DisposeAsync()
        { _stop.Cancel(); if (_writer is not null) await _writer; _mapping?.Dispose(); _stop.Dispose(); }
    }

    private async Task RunThumbnailIdentityAsync(string nativePath)
    {
        var phases = new List<object>();
        string? error = null;
        var sources = Enumerable.Range(0, 3).Select(i => new IdentitySource(i)).ToArray();
        var supervisor = new MediaCoreSupervisor(new()
        {
            Command = Path.GetFullPath(nativePath), MaxRestarts = 0,
            Environment = new Dictionary<string, string>
            { ["COREVIDEO_ISOLATE_MONITORS"] = "1", ["COREVIDEO_GPU_CAPTURE"] = "0" }
        });
        await using var bridge = new MediaCoreBridgeService(supervisor);
        var studio = new StudioViewModel(StudioStartupBootstrap.Create(
            new InMemoryProductionOutputPreferencesStore(), bridge, startInteractiveServices: false, allowMediaCore: true));
        var window = new Window();
        try
        {
            foreach (var source in sources) source.Start();
            await bridge.StartAsync();
            foreach (var source in sources)
                await bridge.RegisterCaptureShmAsync(source.Id, source.MappingName, IdentitySource.Width, IdentitySource.Height);
            var devices = sources.Select(source => new CaptureDevice
            {
                Id = source.Id, Name = source.Id, NativeDeviceId = source.Id, Vendor = "uvc",
                Inputs = [], SelectedInputId = "", ConnectionState = CaptureConnectionState.Connected
            }).ToArray();
            foreach (var device in devices) studio.CaptureDevices.Add(device);
            void RefreshChoices()
            { foreach (var input in studio.ShowInputEditors) input.RefreshSourceOptions([], devices); }
            RefreshChoices();
            using (ShowInputWriteScope.Enter("source-identity-probe"))
                for (var i = 0; i < 3; i++)
                { studio.ShowInputEditors[i].SelectedUnifiedSourceId = "capture:" + sources[i].Id; studio.ShowInputEditors[i].InShow = true; }
            var root = new StudioWorkspace { ViewModel = studio };
            window.Content = root;
            window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1600, 950));
            window.AppWindow.Show(false);
            await Until(() => root.IsLoaded, 5000);
            await Task.Delay(1000);
            studio.ActiveTab = StudioTab.Inputs;
            await Until(() => Descendants(root).OfType<SourcesWorkspaceControl>().Any(w => w.IsLoaded), 5000);
            var workspace = Descendants(root).OfType<SourcesWorkspaceControl>().Single();
            var rows = Descendants(workspace).OfType<SourcesTableRowControl>().OrderBy(r => r.Input.SlotNumber).ToArray();
            var hosts = rows.Take(3).Select(row => Descendants(row).OfType<VideoSurfaceHost>().Single()).ToArray();
            var originalScene = studio.ActiveSceneId;

            async Task Observe(string phase, int?[] identities)
            {
                await Until(() => hosts.Select((host, i) => identities[i] is null
                    ? !host.IsEffectivelyVisible && !rows[i].Input.Inspector.HasPreviewTile
                    : host.IsEffectivelyVisible && host.IsGpuPathActive &&
                      host.SurfaceKey == "source-thumbnail:capture:" + sources[identities[i]!.Value].Id).All(v => v), 15000);
                // The existing one-shot readback returns the last completed probe.
                // Request a fresh sample and allow it to retire before judging a new binding.
                foreach (var host in hosts) host.ReadPixelForProbe();
                await Task.Delay(100);
                var values = new HashSet<uint>[] { [], [], [] };
                var before = hosts.Select(h => h.PresentedFramesForProbe).ToArray();
                var clock = Stopwatch.StartNew();
                while (clock.ElapsedMilliseconds < 1500)
                {
                    for (var i = 0; i < hosts.Length; i++)
                    {
                        if (identities[i] is not { } expected) continue;
                        var host = hosts[i];
                        if (host.SurfaceKey != "source-thumbnail:capture:" + sources[expected].Id)
                            throw new InvalidOperationException($"{phase}: row {i + 1} changed identity");
                        if (host.ReadPixelForProbe() is not { } pixel) continue;
                        values[i].Add(pixel);
                        var channels = new[] { (pixel & 255), (pixel >> 8) & 255, (pixel >> 16) & 255 };
                        if (channels[expected] < 140 || channels.Where((_, c) => c != expected).Any(v => v > 15))
                            throw new InvalidOperationException($"{phase}: row {i + 1} expected channel {expected}, presented {pixel:X8}");
                        if (host.SurfaceState?.NaturalSourceWidth != IdentitySource.Width || host.SurfaceState.NaturalSourceHeight != IdentitySource.Height)
                            throw new InvalidOperationException($"{phase}: source dimensions changed");
                    }
                    await Task.Delay(30);
                }
                for (var i = 0; i < hosts.Length; i++)
                    if (identities[i] is not null && (values[i].Count < 5 || hosts[i].PresentedFramesForProbe <= before[i] + 5))
                        throw new InvalidOperationException($"{phase}: row {i + 1} did not present changing pictures");
                if (!rows.SequenceEqual(Descendants(workspace).OfType<SourcesTableRowControl>().OrderBy(r => r.Input.SlotNumber)) || studio.ActiveSceneId != originalScene)
                    throw new InvalidOperationException($"{phase}: row identity or Program changed");
                phases.Add(new { phase, durationMs = clock.Elapsed.TotalMilliseconds, expectedChannels = identities,
                    distinctPixels = values.Select(v => v.Count).ToArray(),
                    presentedFrames = hosts.Select((h, i) => h.PresentedFramesForProbe - before[i]).ToArray() });
            }

            void Pick(int row, int source)
            {
                RefreshChoices();
                var picker = Descendants(rows[row]).OfType<SourceAssignmentPicker>().Single();
                picker.PrepareMenu();
                var label = rows[row].Input.UnifiedSourceOptions.Single(o => o.Value == "capture:" + sources[source].Id).Label;
                var item = picker.SourceMenu.Items.OfType<MenuFlyoutSubItem>().SelectMany(g => g.Items.OfType<MenuFlyoutItem>()).Single(i => i.Text == label);
                ((IInvokeProvider)new MenuFlyoutItemAutomationPeer(item).GetPattern(PatternInterface.Invoke)).Invoke();
                rows[row].Input.InShow = true;
            }

            await Observe("initial three independent sources", [0, 1, 2]);
            studio.UnassignShowInputCommand.Execute(rows[1].Input);
            await Observe("middle input removed", [0, null, 2]);
            Pick(1, 2);
            await Observe("middle input reused for third source", [0, 2, 2]);
            Pick(0, 1); Pick(1, 0);
            await Observe("first two assignments exchanged", [1, 0, 2]);
            studio.ActiveTab = StudioTab.Studio;
            await Task.Delay(500);
            studio.ActiveTab = StudioTab.Inputs;
            await Observe("Sources reopened", [1, 0, 2]);
            await bridge.UnregisterCaptureShmAsync(sources[1].Id);
            await Until(() => !rows[0].Input.Inspector.HasPreviewTile, 10000);
            phases.Add(new { phase = "first source disconnected", thumbnailCleared = true });
            await bridge.RegisterCaptureShmAsync(sources[1].Id, sources[1].MappingName, IdentitySource.Width, IdentitySource.Height);
            await Observe("source recovered without row replacement", [1, 0, 2]);
        }
        catch (Exception ex) { error = ex.ToString(); }
        finally
        {
            await studio.DisposeAsync();
            var root = window.Content as FrameworkElement;
            window.Content = null;
            if (root is not null) await Until(() => !root.IsLoaded, 3000);
            for (var i = 0; i < 2; i++)
            {
                var drained = new TaskCompletionSource();
                window.DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () => drained.SetResult());
                await drained.Task;
            }
            foreach (var source in sources) await source.DisposeAsync();
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath, JsonSerializer.Serialize(new
            {
                passed = error is null, phases, error, physicalDisplayVerified = false,
                everyTransitionalFrameVerified = false, pixelSampling = "existing one-pixel presenter probe; 30 ms requests after 100 ms acquisition settling"
            }, new JsonSerializerOptions { WriteIndented = true }));
            if (error is not null) Environment.ExitCode = 1;
            window.Close();
        }
    }
}
