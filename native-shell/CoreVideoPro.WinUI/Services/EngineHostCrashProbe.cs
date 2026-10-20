using System.Text.Json;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.Services;

// Explicit isolated verification mode: real compiled WinUI process + production
// bridge; optional full Studio workspace with isolated preferences, no activation or device registration.
internal static class EngineHostCrashProbe
{
    public static async Task RunAsync(string configPath, bool fullStudio = false)
    {
        using var configuration = JsonDocument.Parse(await File.ReadAllTextAsync(configPath));
        var config = configuration.RootElement;
        string Text(string name) => config.GetProperty(name).GetString()!;
        var report = Path.GetFullPath(Text("report"));
        var supervisor = new MediaCoreSupervisor(new()
        {
            EngineHost = new EngineHostClientOptions(Text("host"), Text("core"), Text("profile"))
        });
        var resourceEvents = 0;
        supervisor.ProgramSharedTextureReceived += _ => Interlocked.Increment(ref resourceEvents);
        await using var bridge = new MediaCoreBridgeService(supervisor);
        using var deadline = new CancellationTokenSource(TimeSpan.FromMinutes(3));
        CoreVideoPro.WinUI.ViewModels.StudioViewModel? studio = null;
        Microsoft.UI.Xaml.Window? window = null;
        CoreVideoPro.WinUI.Views.ProgramPreviewPopoutWindow? monitors = null;
        CoreVideoPro.WinUI.Views.StudioWorkspace? workspace = null;
        if (fullStudio)
        {
            Environment.SetEnvironmentVariable("COREVIDEO_GRAPHICS_DOCUMENT_PATH", Path.Combine(Path.GetDirectoryName(report)!, "isolated-graphics.json"));
            studio = new(StudioStartupBootstrap.Create(new InMemoryProductionOutputPreferencesStore(), bridge,
                startInteractiveServices: false, allowMediaCore: true));
            workspace = new() { ViewModel = studio };
            window = new() { Content = workspace };
            window.AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(-20000, -20000, 1920, 1080));
            window.AppWindow.Show(false);
            await studio.StartMediaCoreOnLaunchAsync();
            monitors = new(studio);
            monitors.InitializeOffscreenBindings();
            monitors.AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(-20000, -20000, 1280, 520));
            monitors.AppWindow.Show(false);
            while (!workspace.IsLoaded) { deadline.Token.ThrowIfCancellationRequested(); await Task.Delay(30, deadline.Token); }
        }
        else await bridge.StartAsync(deadline.Token);
        if (config.TryGetProperty("takeControl", out var takeover) && takeover.GetBoolean())
            await supervisor.HostedControlAsync("take-control", deadline.Token);
        if (config.TryGetProperty("commands", out var commands))
        {
            foreach (var command in commands.EnumerateArray())
            {
                var payload = JsonSerializer.Deserialize<Dictionary<string, object?>>(command.GetRawText())!;
                using var response = await supervisor.ForwardHostedCommandAsync(payload, deadline.Token);
                if (response.RootElement.TryGetProperty("ok", out var ok) && !ok.GetBoolean())
                    throw new InvalidOperationException("Probe command rejected: " + response.RootElement.GetRawText());
            }
        }
        var seconds = config.TryGetProperty("seconds", out var duration) ? duration.GetInt32() : 5;
        if (seconds is < 1 or > 120) throw new ArgumentOutOfRangeException("seconds");
        var end = DateTimeOffset.UtcNow.AddSeconds(seconds);
        Directory.CreateDirectory(Path.GetDirectoryName(report)!);
        do
        {
            using var snapshot = await supervisor.ForwardHostedCommandAsync(new() { ["type"] = "snapshot" }, deadline.Token);
            var json = JsonSerializer.Serialize(new { uiPid = Environment.ProcessId, corePid = supervisor.HostedProcessId, hostEpoch = supervisor.HostedEpoch,
                readOnly = supervisor.HostedReadOnly, fullStudio, workspaceLoaded = workspace?.IsLoaded, engineStatus = studio?.EngineStatus, resourceEvents, programSurface = studio?.ProgramSurface, gpuSurfaces = workspace is null ? [] : GpuSurfaces(workspace).Concat(GpuSurfaces(monitors!.Content)).ToArray(), at = DateTimeOffset.UtcNow, snapshot = snapshot.RootElement });
            await File.WriteAllTextAsync(report + ".tmp", json, deadline.Token);
            File.Move(report + ".tmp", report, true);
            await Task.Delay(500, deadline.Token);
        } while (DateTimeOffset.UtcNow < end);
        if (config.TryGetProperty("shutdown", out var shutdown) && shutdown.GetBoolean())
            await supervisor.HostedControlAsync("shutdown", deadline.Token);
        if (studio is not null) await studio.DisposeAsync();
        monitors?.Close();
        window?.Close();
        if (studio is not null)
            ShutdownCompletion.Complete(true, true, studio.StopDispatcherTimersForShutdown, LaunchLog.Write,
                ShutdownCompletion.TerminateCurrentProcess, () => ApplicationLifecycle.ForceExit());
    }
    private static IEnumerable<object> GpuSurfaces(Microsoft.UI.Xaml.DependencyObject root)
    {
        if (root is CoreVideoPro.WinUI.Controls.VideoSurfaceHost surface)
            yield return new { surface.SurfaceKey, surface.IsGpuPathActive, surface.IsLoaded };
        for (var i = 0; i < Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChildrenCount(root); i++)
            foreach (var child in GpuSurfaces(Microsoft.UI.Xaml.Media.VisualTreeHelper.GetChild(root, i))) yield return child;
    }

}
