using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Views;
using Microsoft.UI.Xaml;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

internal sealed partial class SourceInspectVideoProbe
{
    private async Task RunNativeAsync(string nativePath)
    {
        string? error = null;
        var samples = new List<object>();
        var fakePath = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(nativePath))!, "corevideo-zoom-engine-fake.exe");
        if (!File.Exists(fakePath)) throw new FileNotFoundException("Synthetic engine required; this probe must never join real Zoom", fakePath);
        var supervisor = new MediaCoreSupervisor(new()
        {
            Command = Path.GetFullPath(nativePath), MaxRestarts = 0,
            Environment = new Dictionary<string, string> { ["COREVIDEO_ISOLATE_MONITORS"] = "1", ["COREVIDEO_GPU_CAPTURE"] = "0",
                ["COREVIDEO_ZOOM_ENGINE_PATH"] = fakePath, ["COREVIDEO_FAKE_ENGINE_PARTICIPANTS"] = "10",
                ["COREVIDEO_FAKE_ENGINE_FPS"] = "60", ["COREVIDEO_FAKE_ENGINE_RES"] = "2" }
        });
        await using var bridge = new MediaCoreBridgeService(supervisor);
        var fullStartup = StudioStartupBootstrap.Create(new InMemoryProductionOutputPreferencesStore(), bridge, startInteractiveServices: false);
        await using var studio = new StudioViewModel(StudioStartupBootstrap.Create(
            new InMemoryProductionOutputPreferencesStore(), startInteractiveServices: false));
        const int width = 320, height = 180;
        var pixels = Enumerable.Range(0, width * height).SelectMany(_ => new byte[] { 0, 0, 255, 255 }).ToArray();
        var mappingName = "Local\\CoreVideo-Inspect-Probe-" + Guid.NewGuid().ToString("N");
        using var mapping = MemoryMappedFile.CreateNew(mappingName, 16 + pixels.Length);
        using var source = mapping.CreateViewAccessor();
        source.Write(0, 2u); source.Write(4, width); source.Write(8, height); source.WriteArray(16, pixels, 0, pixels.Length);
        var color = 0; uint sequence = 2;
        using var stop = new CancellationTokenSource();
        var publisher = Task.Run(async () =>
        {
            while (!stop.IsCancellationRequested)
            {
                var blue = Volatile.Read(ref color) != 0;
                for (var i = 0; i < pixels.Length; i += 4) { pixels[i] = blue ? (byte)255 : (byte)0; pixels[i + 2] = blue ? (byte)0 : (byte)255; }
                sequence += 2; source.Write(0, sequence - 1); source.WriteArray(16, pixels, 0, pixels.Length); source.Write(0, sequence);
                await Task.Delay(33);
            }
        });
        var window = new Window();
        var latest = new Dictionary<string, string>();
        long received = 0;
        var zoomPhase = false;
        var fullShell = false;
        StudioViewModel? fullStudio = null;
        bridge.ParticipantSharedTextureReceived += texture =>
        {
            Interlocked.Increment(ref received);
            if (fullShell) return;
            window.DispatcherQueue.TryEnqueue(() =>
            {
                var indexText = zoomPhase ? texture.ParticipantId : texture.ParticipantId.Replace("capture:inspect-", "");
                if (!int.TryParse(indexText, out var index)) return;
                if (zoomPhase) index -= 101;
                if (index is < 0 or >= 10) return;
                if (latest.TryGetValue(texture.ParticipantId, out var old) && old == texture.SharedHandleHex) return;
                latest[texture.ParticipantId] = texture.SharedHandleHex;
                var row = studio.ShowInputEditors[index].Inspector;
                row.PreviewSurface = VideoSurfaceState.Waiting(VideoSurfaceKind.Participant,
                    "source-thumbnail:" + texture.ParticipantId, texture.ParticipantId) with
                {
                    PendingSharedHandle = new() { NtHandle = CoreProtocolParser.ParseSharedHandleHex(texture.SharedHandleHex),
                        Width = texture.Width, Height = texture.Height, Format = texture.Format, FrameNumber = texture.FrameNumber },
                    NaturalSourceWidth = texture.Width, NaturalSourceHeight = texture.Height
                };
                row.HasPreviewTile = true;
            });
        };
        static NativeMediaCoreCommand Command(string type, object fields) => new()
        { Type = type, ExtensionData = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(JsonSerializer.Serialize(fields)) };
        try
        {
            var workspace = new SourcesWorkspaceControl(new Columns()) { Studio = studio };
            window.Content = new Microsoft.UI.Xaml.Controls.ScrollViewer { Content = workspace,
                HorizontalScrollBarVisibility = Microsoft.UI.Xaml.Controls.ScrollBarVisibility.Disabled,
                VerticalScrollBarVisibility = Microsoft.UI.Xaml.Controls.ScrollBarVisibility.Auto };
            window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1600, 950)); window.AppWindow.Show(false);
            await Until(() => workspace.IsLoaded, 3000);
            workspace.State.Inspect(studio.ShowInputEditors[0]);
            var inspector = Descendants(workspace).OfType<VideoSurfaceHost>().Single(h => h.Name == "InspectorSourcePreview");
            var thumbnail = Descendants(Descendants(workspace).OfType<SourcesTableRowControl>().First()).OfType<VideoSurfaceHost>().Single();
            await bridge.StartAsync();
            for (var i = 0; i < 10; i++) await bridge.RegisterCaptureShmAsync("inspect-" + i, mappingName, width, height);
            await bridge.SyncAsync([
                Command("set-output-profile", new { width = 1280, height = 720, fps = 30 }),
                Command("load-scene-graph", new { sceneId = "inspect-probe", routes = new[] { new {
                    routeId = "source", mode = "capture-input", captureDeviceId = "inspect-0", fitMode = "fill", opacity = 1,
                    rect = new { x = 0, y = 0, width = 1, height = 1 } } } })]);
            await Until(() => latest.Count == 10, 15000);
            await Until(() => inspector.ReadPixelForProbe() == 0xffff0000u && thumbnail.ReadPixelForProbe() == 0xffff0000u, 3000);
            var inspectorStart = inspector.PresentedFramesForProbe; var thumbStart = thumbnail.PresentedFramesForProbe;
            await Task.Delay(2200);
            samples.Add(new { idleMs = 2200, inspectFrames = inspector.PresentedFramesForProbe - inspectorStart,
                thumbnailFrames = thumbnail.PresentedFramesForProbe - thumbStart, received = Interlocked.Read(ref received) });
            for (var i = 0; i < 20; i++)
            {
                var expected = i % 2 == 0 ? 0xff0000ffu : 0xffff0000u;
                var clock = Stopwatch.StartNew(); Volatile.Write(ref color, i % 2 == 0 ? 1 : 0);
                await Until(() => inspector.ReadPixelForProbe() == expected && thumbnail.ReadPixelForProbe() == expected, 1500);
                samples.Add(new { trial = i, bothPixelChangeMs = clock.Elapsed.TotalMilliseconds, expected = expected.ToString("X8") });
                await Task.Delay(50);
            }
            zoomPhase = true;
            await bridge.JoinZoomAsync("https://zoom.us/j/1234567890", "Owned synthetic Inspect QA", false, sdkJwt: "synthetic");
            await Until(() => latest.ContainsKey("101"), 15000);
            await Until(() => inspector.ReadPixelForProbe() is { } pixel && pixel != 0xffff0000u, 3000);
            var distinctInspect = new HashSet<uint>(); var distinctThumbnail = new HashSet<uint>();
            var motion = Stopwatch.StartNew();
            while (motion.ElapsedMilliseconds < 3000)
            {
                if (inspector.ReadPixelForProbe() is { } inspectPixel) distinctInspect.Add(inspectPixel);
                if (thumbnail.ReadPixelForProbe() is { } thumbnailPixel) distinctThumbnail.Add(thumbnailPixel);
                await Task.Delay(50);
            }
            samples.Add(new { phase = "synthetic Zoom I420 1080p60", durationMs = motion.Elapsed.TotalMilliseconds,
                distinctInspectPixels = distinctInspect.Count, distinctThumbnailPixels = distinctThumbnail.Count });
            if (distinctInspect.Count < 20 || distinctThumbnail.Count < 20)
                throw new InvalidOperationException("Synthetic Zoom did not present continuous changing pixels in both views");

            fullShell = true;
            window.Content = null;
            fullStudio = new StudioViewModel(fullStartup);
            using (ShowInputWriteScope.Enter("isolated-inspect-qa"))
                for (var i = 0; i < 10; i++)
                {
                    fullStudio.ShowInputs[i].Kind = ShowInputKind.ZoomParticipant;
                    fullStudio.ShowInputs[i].ParticipantId = (101 + i).ToString();
                    fullStudio.ShowInputs[i].InShow = true;
                }
            fullStudio.ActiveTab = StudioTab.Inputs;
            var root = new StudioWorkspace { ViewModel = fullStudio };
            window.Content = root;
            await Until(() => root.IsLoaded, 5000);
            var fullWorkspace = Descendants(root).OfType<SourcesWorkspaceControl>().Single();
            fullWorkspace.State.Inspect(fullStudio.ShowInputEditors[0]);
            await bridge.SyncAsync([]);
            var fullInspector = Descendants(fullWorkspace).OfType<VideoSurfaceHost>().Single(h => h.Name == "InspectorSourcePreview");
            var fullThumbnail = Descendants(Descendants(fullWorkspace).OfType<SourcesTableRowControl>().First()).OfType<VideoSurfaceHost>().Single();
            await Until(() => fullInspector.ReadPixelForProbe() is not null && fullThumbnail.ReadPixelForProbe() is not null, 15000);
            distinctInspect.Clear(); distinctThumbnail.Clear(); motion.Restart();
            while (motion.ElapsedMilliseconds < 6000)
            {
                if (fullInspector.ReadPixelForProbe() is { } ip) distinctInspect.Add(ip);
                if (fullThumbnail.ReadPixelForProbe() is { } tp) distinctThumbnail.Add(tp);
                await Task.Delay(100);
            }
            samples.Add(new { phase = "complete StudioWorkspace and live model", durationMs = motion.Elapsed.TotalMilliseconds,
                distinctInspectPixels = distinctInspect.Count, distinctThumbnailPixels = distinctThumbnail.Count });
            if (distinctInspect.Count < 20 || distinctThumbnail.Count < 20)
                throw new InvalidOperationException("Complete shell did not present continuous changing pixels in both views");
        }
        catch (Exception ex) { error = ex.ToString(); }
        finally
        {
            stop.Cancel(); await publisher;
            if (fullStudio is not null) await fullStudio.DisposeAsync();
            var retiringRoot = window.Content as FrameworkElement;
            window.Content = null;
            if (retiringRoot is not null) await Until(() => !retiringRoot.IsLoaded, 3000);
            // Let unload callbacks finish while their XAML window is still valid.
            // Participant-list clear schedules a second low-priority rebind.
            for (var phase = 0; phase < 2; phase++)
            {
                var drained = new TaskCompletionSource();
                window.DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () => drained.SetResult());
                await drained.Task;
            }
            window.Close();
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath, JsonSerializer.Serialize(new { passed = error is null,
                samples, sourceCount = latest.Count, received, physicalDisplayVerified = false, error }, new JsonSerializerOptions { WriteIndented = true }));
            if (error is not null) Environment.ExitCode = 1;
        }
    }
}
