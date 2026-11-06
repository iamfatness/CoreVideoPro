using System.Diagnostics;
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
    private async Task RunThumbnailIdentityAsync(string nativePath)
    {
        var phases = new List<object>();
        string? error = null;
        var sources = Enumerable.Range(101, 3).Select(i => i.ToString()).ToArray();
        var fakePath = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(nativePath))!, "corevideo-zoom-engine-fake.exe");
        if (!File.Exists(fakePath)) throw new FileNotFoundException("Owned synthetic engine required", fakePath);
        var supervisor = new MediaCoreSupervisor(new()
        {
            Command = Path.GetFullPath(nativePath), MaxRestarts = 0,
            Environment = new Dictionary<string, string>
            { ["COREVIDEO_ISOLATE_MONITORS"] = "1", ["COREVIDEO_GPU_CAPTURE"] = "0",
              ["COREVIDEO_ZOOM_ENGINE_PATH"] = fakePath, ["COREVIDEO_FAKE_ENGINE_PARTICIPANTS"] = "3",
              ["COREVIDEO_FAKE_ENGINE_RES"] = "2", ["COREVIDEO_FAKE_ENGINE_FPS"] = "60",
              ["COREVIDEO_FAKE_NO_CHURN"] = "1", ["COREVIDEO_FAKE_ENGINE_AUTOSUBSCRIBE"] = "0",
              ["COREVIDEO_FAKE_PRIMARY_COLORS"] = "1" }
        });
        await using var bridge = new MediaCoreBridgeService(supervisor);
        var studio = new StudioViewModel(StudioStartupBootstrap.Create(
            new InMemoryProductionOutputPreferencesStore(), bridge, startInteractiveServices: false, allowMediaCore: true));
        var window = new Window();
        try
        {
            await bridge.StartAsync();
            await bridge.JoinZoomAsync("https://zoom.us/j/1234567890", "Owned source identity QA", false, sdkJwt: "synthetic");
            await studio.ToggleEngineCommand.ExecuteAsync(null);
            await Until(() => studio.RoomParticipantsForInputs.Count >= 3, 10000);
            void RefreshChoices()
            { foreach (var input in studio.ShowInputEditors) input.RefreshSourceOptions(studio.RoomParticipantsForInputs, studio.CaptureDevices); }
            RefreshChoices();
            using (ShowInputWriteScope.Enter("source-identity-probe"))
                for (var i = 0; i < 3; i++)
                { studio.ShowInputEditors[i].SelectedUnifiedSourceId = "zoom:" + sources[i]; studio.ShowInputEditors[i].InShow = true; }
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
                try { await Until(() => hosts.Select((host, i) => identities[i] is null
                    ? !host.IsEffectivelyVisible && !rows[i].Input.Inspector.HasPreviewTile
                    : host.IsEffectivelyVisible && host.IsGpuPathActive &&
                      host.SurfaceKey == "source-thumbnail:zoom:" + sources[identities[i]!.Value]).All(v => v), 15000); }
                catch
                {
                    phases.Add(new { phase, ready = false, activeTab = studio.ActiveTab.ToString(),
                        rows = rows.Take(3).Select((row, i) => new { row.Input.SourceId, row.Input.InShow,
                            row.Input.Inspector.StatusLabel, row.Input.Inspector.HasPreviewTile,
                            hosts[i].IsEffectivelyVisible, hosts[i].IsGpuPathActive, hosts[i].SurfaceKey,
                            diagnostics = hosts[i].DiagnosticsForProbe }).ToArray(),
                        nativeSources = bridge.LastSnapshot?.Sources });
                    throw;
                }
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
                        if (host.SurfaceKey != "source-thumbnail:zoom:" + sources[expected])
                            throw new InvalidOperationException($"{phase}: row {i + 1} changed identity");
                        if (host.ReadPixelForProbe() is not { } pixel) continue;
                        values[i].Add(pixel);
                        var channels = new[] { (pixel & 255), (pixel >> 8) & 255, (pixel >> 16) & 255 };
                        // Limited-range I420 plus the moving luma ramp leaves small
                        // secondary components after conversion. These disjoint bands
                        // distinguish the three primaries without demanding RGB zeros.
                        if (channels[expected] < 180 || channels.Where((_, c) => c != expected).Any(v => v > 64))
                            throw new InvalidOperationException($"{phase}: row {i + 1} expected channel {expected}, presented {pixel:X8}");
                        if (host.SurfaceState?.NaturalSourceWidth != 1920 || host.SurfaceState.NaturalSourceHeight != 1080)
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
                var label = rows[row].Input.UnifiedSourceOptions.Single(o => o.Value == "zoom:" + sources[source]).Label;
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
            await bridge.LeaveZoomAsync();
            await Until(() => rows.Take(3).All(row => !row.Input.Inspector.HasPreviewTile), 10000);
            phases.Add(new { phase = "synthetic meeting departed", thumbnailsCleared = true });
            await bridge.JoinZoomAsync("https://zoom.us/j/1234567890", "Owned source identity QA", false, sdkJwt: "synthetic");
            if (!studio.ZoomCaptureSubscribed) await studio.ToggleEngineCommand.ExecuteAsync(null);
            await Observe("new source lifetimes recovered without row replacement", [1, 0, 2]);
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
