using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using CoreVideoPro.WinUI.Controls;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.Mathematics;
using Windows.Graphics;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Owned GPU sources and offscreen real controls; never connects to an installed core.</summary>
internal sealed partial class SourceInspectVideoProbe(string reportPath, string? corePath = null)
{
    private sealed class Columns : ISourcesWorkspacePreferenceStore
    {
        public SourcesWorkspacePreferences Load() => new();
        public void Save(SourcesWorkspacePreferences value) { }
    }
    private sealed class Producer : IDisposable
    {
        private readonly ID3D11Device _device = D3D11.D3D11CreateDevice(DriverType.Hardware, DeviceCreationFlags.BgraSupport);
        private readonly ID3D11DeviceContext _context;
        private readonly ID3D11Texture2D _texture;
        private readonly ID3D11RenderTargetView _target;
        private readonly IDXGIKeyedMutex _mutex;
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int Acquire(nint self, ulong key, uint wait);
        private readonly Acquire _acquire;
        public SharedTextureHandle Handle { get; }
        public Producer()
        {
            _context = _device.ImmediateContext;
            _texture = _device.CreateTexture2D(new Texture2DDescription
            {
                Width = 1920, Height = 1080, MipLevels = 1, ArraySize = 1,
                Format = Format.B8G8R8A8_UNorm, SampleDescription = new(1, 0),
                Usage = ResourceUsage.Default, BindFlags = BindFlags.RenderTarget | BindFlags.ShaderResource,
                MiscFlags = ResourceOptionFlags.SharedKeyedMutex
            });
            _target = _device.CreateRenderTargetView(_texture);
            _mutex = _texture.QueryInterface<IDXGIKeyedMutex>();
            _acquire = Marshal.GetDelegateForFunctionPointer<Acquire>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(_mutex.NativePointer), 8 * IntPtr.Size));
            using var resource = _texture.QueryInterface<IDXGIResource>();
            Handle = new() { NtHandle = unchecked((ulong)resource.SharedHandle), Width = 1920, Height = 1080, Format = "BGRA8", FrameNumber = 1 };
        }
        public void Publish(Color4 color)
        {
            if (_acquire(_mutex.NativePointer, 0, 0) != 0 && _acquire(_mutex.NativePointer, 1, 0) != 0) return;
            _context.ClearRenderTargetView(_target, color);
            _context.Flush();
            _mutex.ReleaseSync(1);
        }
        public void Dispose() { _mutex.Dispose(); _target.Dispose(); _texture.Dispose(); _context.Dispose(); _device.Dispose(); }
    }
    internal async Task RunAsync()
    {
        Direct3D11InteropService.CollectProbeCounters = true;
        if (corePath is not null) { await RunNativeAsync(corePath); return; }
        string? error = null;
        var samples = new List<object>();
        var window = new Window();
        await using var studio = new StudioViewModel(StudioStartupBootstrap.Create(new InMemoryProductionOutputPreferencesStore(), startInteractiveServices: false));
        using var red = new Producer(); using var green = new Producer();
        var others = Enumerable.Range(0, 8).Select(_ => new Producer()).ToArray();
        using var producerStop = new CancellationTokenSource();
        var redIsBlue = 0;
        // Independent producer: source activity must not depend on UI invalidation.
        var producer = Task.Run(async () =>
        {
            while (!producerStop.IsCancellationRequested)
            {
                red.Publish(Volatile.Read(ref redIsBlue) == 0 ? new Color4(1, 0, 0, 1) : new Color4(0, 0, 1, 1));
                green.Publish(new Color4(0, 1, 0, 1));
                foreach (var other in others) other.Publish(new Color4(0, 1, 0, 1));
                await Task.Delay(16);
            }
        });
        try
        {
            var slot = studio.ShowInputEditors[0];
            var workspace = new SourcesWorkspaceControl(new Columns()) { Studio = studio };
            window.Content = new Microsoft.UI.Xaml.Controls.ScrollViewer { Content = workspace,
                HorizontalScrollBarVisibility = Microsoft.UI.Xaml.Controls.ScrollBarVisibility.Disabled,
                VerticalScrollBarVisibility = Microsoft.UI.Xaml.Controls.ScrollBarVisibility.Auto }; window.AppWindow.MoveAndResize(new RectInt32(-20000, -20000, 1600, 950)); window.AppWindow.Show(false);
            await Until(() => workspace.IsLoaded, 3000);
            var inspector = Descendants(workspace).OfType<VideoSurfaceHost>().Single(host => host.Name == "InspectorSourcePreview");
            VideoSurfaceState State(string id, SharedTextureHandle handle) => VideoSurfaceState.Waiting(VideoSurfaceKind.Participant, "source-thumbnail:" + id, id) with { PendingSharedHandle = handle, NaturalSourceWidth = 1920, NaturalSourceHeight = 1080 };
            var a = State("probe-red", red.Handle); var b = State("probe-green", green.Handle);
            for (var i = 0; i < studio.ShowInputEditors.Count; i++)
            {
                var handle = i == 0 ? red.Handle : i == 1 ? green.Handle : others[i - 2].Handle;
                studio.ShowInputEditors[i].Inspector.PreviewSurface = State("probe-" + i, handle);
                studio.ShowInputEditors[i].Inspector.HasPreviewTile = true;
            }
            for (var i = 0; i < 12; i++)
            {
                workspace.State.InspectorOpen = false;
                var state = i % 2 == 0 ? a : b; var expected = i % 2 == 0 ? 0xffff0000u : 0xff00ff00u;
                // Same sequence as selecting an input: binding changes before the inspector becomes visible.
                var selected = studio.ShowInputEditors[i % 2];
                selected.Inspector.PreviewSurface = state; selected.Inspector.HasPreviewTile = true;
                workspace.State.SelectedInput = selected;
                var clock = Stopwatch.StartNew();
                workspace.State.InspectorOpen = true; workspace.UpdateLayout();
                await Until(() => inspector.PictureVisibleForProbe && inspector.PresentedFramesForProbe > 0 && inspector.ReadPixelForProbe() == expected, 1500);
                samples.Add(new { trial = i, firstPixelMs = clock.Elapsed.TotalMilliseconds, expected = expected.ToString("X8"), presented = inspector.PresentedFramesForProbe });
                await Task.Delay(8);
            }
            workspace.State.InspectorOpen = false;
            slot.Inspector.PreviewSurface = a; slot.Inspector.HasPreviewTile = true; workspace.State.SelectedInput = slot; workspace.State.InspectorOpen = true;
            await Until(() => inspector.ReadPixelForProbe() == 0xffff0000u, 1500);
            var idleStart = inspector.PresentedFramesForProbe;
            await Task.Delay(2200); // No UI timer, polling or property invalidation during this interval.
            var idleFrames = inspector.PresentedFramesForProbe - idleStart;
            samples.Add(new { continuity = "idle UI with independent producer", durationMs = 2200, presented = idleFrames });
            if (idleFrames < 20) throw new InvalidOperationException("Inspect presentation stopped when UI was idle: " + idleFrames + " frames in 2200ms");
            for (var i = 0; i < 20; i++)
            {
                var startCount = inspector.PresentedFramesForProbe;
                var moving = Stopwatch.StartNew();
                var blue = i % 2 == 0;
                Volatile.Write(ref redIsBlue, blue ? 1 : 0); // No binding, roster or format event.
                var expected = blue ? 0xff0000ffu : 0xffff0000u;
                await Until(() => inspector.PictureVisibleForProbe && inspector.ReadPixelForProbe() == expected, 1500);
                samples.Add(new { continuity = "stable handle changing pixels", trial = i,
                    pixelChangeMs = moving.Elapsed.TotalMilliseconds, framesAdvanced = inspector.PresentedFramesForProbe - startCount });
                await Task.Delay(100);
            }

        }
        catch (Exception ex) { error = ex.ToString(); }
        finally
        {
            producerStop.Cancel(); await producer; window.Content = null; window.Close();
            foreach (var other in others) other.Dispose();
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(reportPath))!);
            await File.WriteAllTextAsync(reportPath, JsonSerializer.Serialize(new { passed = error is null, samples, physicalDisplayVerified = false, error }, new JsonSerializerOptions { WriteIndented = true }));
            if (error is not null) Environment.ExitCode = 1;
        }
    }
    private static async Task Until(Func<bool> condition, int timeout)
    { var clock = Stopwatch.StartNew(); while (!condition()) { if (clock.ElapsedMilliseconds > timeout) throw new InvalidOperationException("Inspect did not present the expected GPU pixel within " + timeout + "ms"); await Task.Delay(5); } }
    private static IEnumerable<DependencyObject> Descendants(DependencyObject root)
    { for (var i = 0; i < VisualTreeHelper.GetChildrenCount(root); i++) { var child = VisualTreeHelper.GetChild(root, i); yield return child; foreach (var next in Descendants(child)) yield return next; } }
}
