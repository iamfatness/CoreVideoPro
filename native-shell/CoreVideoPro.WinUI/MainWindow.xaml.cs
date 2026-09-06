using CoreVideoPro.Control.Http;
using CoreVideoPro.Control.Osc;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using System.Net;
using WinRT.Interop;

namespace CoreVideoPro.WinUI;

public sealed partial class MainWindow : Window
{
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ShutdownWatchdog = TimeSpan.FromSeconds(6);
    private static readonly TimeSpan ResourceMonitorInterval = TimeSpan.FromMilliseconds(750);

    private readonly AppWindow _appWindow;
    private readonly SystemResourceMonitorService _resourceMonitor = new();
    private readonly DispatcherQueue? _dispatcher = DispatcherQueue.GetForCurrentThread();
    private DispatcherQueueTimer? _resourceMonitorTimer;
    private StudioControlSurface? _controlSurface;
    private OscControlServer? _controlServer;
    private HttpControlServer? _httpControlServer;
    private UpdateNotificationService.UpdateOffer? _updateOffer;
    private bool _resourceMonitoringStopped;
    private bool _shutdownStarted;
    private bool _allowWindowClose;
    // App releases its window reference on Closed; the fallback must outlive it.
    private static System.Threading.Timer? _shutdownWatchdogTimer;

    public StudioViewModel ViewModel { get; }

    internal bool IsShuttingDown => _shutdownStarted || _allowWindowClose;

    public MainWindow()
    {
        ViewModel = new StudioViewModel();
        InitializeComponent();
        RootContent.ViewModel = ViewModel;

        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        _appWindow = AppWindow.GetFromWindowId(windowId)
                     ?? throw new InvalidOperationException("Could not resolve the main AppWindow.");

        Title = "CoreVideo Pro";
        // Merge the branded nav into the title bar so there is no separate
        // Windows caption strip duplicating "CoreVideo Pro" above the logo.
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(RootContent.TitleBarDragRegion);
        WindowChromeService.Apply(this, _appWindow, extendTitleBar: true);
        Activated += OnWindowActivated;
        RootContent.Loaded += OnRootContentLoaded;
        _appWindow.Closing += OnAppWindowClosing;
        Closed += OnWindowClosed;

        StartResourceMonitoring();
        StartControlServer();
        StartUpdateCheck();
    }

    // D4 startup version check: delayed, off-thread, one-shot, silent on any
    // failure (spec §7 — the update check is non-blocking). Only a strictly
    // newer, not-previously-dismissed version opens the static InfoBar.
    private void StartUpdateCheck()
    {
        if (_dispatcher is null)
        {
            return;
        }

        UpdateNotificationService.Start(_dispatcher, ShowUpdateBanner);
    }

    private void ShowUpdateBanner(UpdateNotificationService.UpdateOffer offer)
    {
        if (_shutdownStarted)
        {
            return;
        }

        _updateOffer = offer;
        UpdateBanner.Title = $"Update available — v{offer.NewVersion}";
        UpdateBanner.Message =
            $"You are on v{offer.CurrentVersion}. Dismiss to be reminded when the next version ships.";
        UpdateBanner.IsOpen = true;
    }

    private void OnUpdateBannerGetUpdate(object sender, RoutedEventArgs args)
    {
        if (_updateOffer is { } offer)
        {
            UpdateNotificationService.OpenDownload(offer.DownloadUrl);
        }
    }

    private void OnUpdateBannerDismissed(Microsoft.UI.Xaml.Controls.InfoBar sender, object args)
    {
        if (_updateOffer is { } offer)
        {
            UpdateNotificationService.RecordDismissed(offer.NewVersion);
        }
    }

    // Remote control (OSC) so control surfaces / a Bitfocus Companion module can drive the app.
    // Localhost + port 8010 by default; opt into LAN and a custom port via env vars. A bind
    // failure (e.g. port in use) is logged and swallowed — it must never block the app launch.
    private void StartControlServer()
    {
        if (_dispatcher is null)
        {
            return;
        }

        try
        {
            var port = 8010;
            if (int.TryParse(Environment.GetEnvironmentVariable("COREVIDEO_OSC_PORT"), out var configured) &&
                configured is > 0 and < 65536)
            {
                port = configured;
            }

            var oscLanRequested = string.Equals(Environment.GetEnvironmentVariable("COREVIDEO_OSC_LAN"), "1", StringComparison.Ordinal);
            var oscTrustedNetwork = string.Equals(Environment.GetEnvironmentVariable("COREVIDEO_OSC_TRUSTED_NETWORK"), "1", StringComparison.Ordinal);
            var oscLan = oscLanRequested && oscTrustedNetwork;
            if (oscLanRequested && !oscTrustedNetwork)
            {
                LaunchLog.Write("control: OSC remains on loopback. Unauthenticated LAN OSC requires COREVIDEO_OSC_TRUSTED_NETWORK=1 on a trusted network.");
            }

            _controlSurface = new StudioControlSurface(ViewModel, _dispatcher);

            _controlServer = new OscControlServer(_controlSurface, new OscControlServerOptions
            {
                ListenPort = port,
                BindAddress = oscLan ? IPAddress.Any : IPAddress.Loopback
            });
            _controlServer.Start();
            LaunchLog.Write($"control: OSC server listening on {(oscLan ? "0.0.0.0" : "127.0.0.1")}:{_controlServer.BoundPort}");

            // HTTP + WebSocket API sharing the same surface. Loopback needs no privileges;
            // LAN ("+") may require a Windows urlacl and always requires a bearer token.
            var httpLan = string.Equals(Environment.GetEnvironmentVariable("COREVIDEO_HTTP_LAN"), "1", StringComparison.Ordinal);
            var httpPort = 8011;
            if (int.TryParse(Environment.GetEnvironmentVariable("COREVIDEO_HTTP_PORT"), out var httpConfigured) &&
                httpConfigured is > 0 and < 65536)
            {
                httpPort = httpConfigured;
            }

            try
            {
                _httpControlServer = new HttpControlServer(_controlSurface, new HttpControlServerOptions
                {
                    ListenPort = httpPort,
                    Host = httpLan ? "+" : "127.0.0.1",
                    AuthToken = Environment.GetEnvironmentVariable("COREVIDEO_CONTROL_TOKEN")
                });
                _httpControlServer.Start();
                LaunchLog.Write($"control: HTTP/WS API listening on http://{(httpLan ? "+" : "127.0.0.1")}:{httpPort}/ (GET /manifest, /state, /ws; POST /invoke)");
            }
            catch (Exception ex)
            {
                LaunchLog.Write($"control: HTTP/WS API failed to start ({ex.Message}) — OSC still active");
                _httpControlServer = null;
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"control: OSC server failed to start ({ex.Message})");
        }
    }

    private async Task StopControlServerAsync()
    {
        // Its feedback timer and VM subscriptions are UI-owned. Close the
        // command gate before any asynchronous socket teardown or VM disposal.
        TryShutdownStep("control surface", () => _controlSurface?.Dispose());
        _controlSurface = null;
        if (_httpControlServer is not null)
        {
            try
            {
                await _httpControlServer.DisposeAsync().ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                LaunchLog.WriteException("shutdown: control server disposal", ex);
            }

            _httpControlServer = null;
        }

        if (_controlServer is not null)
        {
            try
            {
                await _controlServer.DisposeAsync().ConfigureAwait(true);
            }
            catch (Exception ex)
            {
                LaunchLog.WriteException("shutdown: control server disposal", ex);
            }

            _controlServer = null;
        }

    }

    private void OnWindowActivated(object sender, WindowActivatedEventArgs args)
    {
        if (_shutdownStarted || _allowWindowClose)
        {
            return;
        }

        WindowChromeService.Apply(this, _appWindow);
    }

    private void OnRootContentLoaded(object sender, RoutedEventArgs args)
    {
        if (IsShuttingDown) return;
        WindowChromeService.Apply(this, _appWindow);
    }

    private void StartResourceMonitoring()
    {
        if (_dispatcher is null)
        {
            return;
        }

        _resourceMonitor.ResourcesSampled += PushResourceSample;
        _resourceMonitor.Prime();
        PushResourceSample(
            _resourceMonitor.CpuLoadPercent,
            _resourceMonitor.MemoryLoadPercent,
            _resourceMonitor.DiskLoadPercent);

        _resourceMonitorTimer = _dispatcher.CreateTimer();
        _resourceMonitorTimer.Interval = ResourceMonitorInterval;
        _resourceMonitorTimer.Tick += OnResourceMonitorTick;
        _resourceMonitorTimer.Start();
    }

    private void OnResourceMonitorTick(DispatcherQueueTimer sender, object args) =>
        _resourceMonitor.Sample();

    private void PushResourceSample(int cpu, int memory, int disk) =>
        ViewModel.Transport.ApplySystemResourceSample(cpu, memory, disk);

    private void StopResourceMonitoring()
    {
        if (_resourceMonitoringStopped)
        {
            return;
        }

        _resourceMonitoringStopped = true;

        if (_resourceMonitorTimer is not null)
        {
            _resourceMonitorTimer.Stop();
            _resourceMonitorTimer.Tick -= OnResourceMonitorTick;
            _resourceMonitorTimer = null;
        }

        _resourceMonitor.ResourcesSampled -= PushResourceSample;
        _resourceMonitor.Dispose();
    }

    private void OnAppWindowClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_allowWindowClose)
        {
            return;
        }

        args.Cancel = true;

        if (_shutdownStarted)
        {
            LaunchLog.Write("shutdown: close requested while cleanup is in progress — forcing exit");
            ApplicationLifecycle.ForceExit();
            return;
        }

        _shutdownStarted = true;
        LaunchLog.Write("shutdown: close requested");
        _ = ShutdownAsync();
    }

    private async Task ShutdownAsync()
    {
        // S3: kick the opt-in session-end telemetry FIRST so it runs concurrently
        // with teardown. Fire-and-forget with its own tight timeout — never awaited,
        // never gates the close (spec §S3.4 / §7). No-op unless consent is ON.
        try
        {
            ViewModel.Settings.FlushTelemetrySessionEnd();
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"telemetry: session-end flush skipped ({ex.Message})");
        }

        // Keep the timer rooted until the process actually exits. Disposing it
        // after Close would leave a lingering SDK thread without a fallback.
        _shutdownWatchdogTimer = new System.Threading.Timer(
            _ => ApplicationLifecycle.ForceExit(),
            null,
            ShutdownWatchdog,
            Timeout.InfiniteTimeSpan);

        // Detach XAML and UI timers before starting worker disposal. Each step
        // is independent: a closed control must not skip media-core cleanup.
        TryShutdownStep("activation", ApplicationLifecycle.PrepareShutdown);
        TryShutdownStep("resource monitor", StopResourceMonitoring);
        TryShutdownStep("window chrome", () => WindowChromeService.ClearScheduledReapply(this));
        TryShutdownStep("workspace detach", () => RootContent.ViewModel = null);
        TryShutdownStep("UI preparation", () => ViewModel.PrepareForShutdown());

        var cleanupSucceeded = false;
        try
        {
            // Control sockets and blocking process teardown can drain together.
            // The UI continuation is required for AppWindow handlers and Close.
            await Task.WhenAll(
                    StopControlServerAsync(),
                    Task.Run(async () => await ViewModel.DisposeAsync().ConfigureAwait(false)))
                .WaitAsync(ShutdownTimeout)
                .ConfigureAwait(true);
            cleanupSucceeded = true;
            LaunchLog.Write("shutdown: resources released");
        }
        catch (Exception ex)
        {
            LaunchLog.WriteException("shutdown: cleanup failed; forcing media core stop", ex);
            try
            {
                await Task.Run(() => ViewModel.ForceStopMediaCoreAsync())
                    .WaitAsync(TimeSpan.FromSeconds(1))
                    .ConfigureAwait(true);
            }
            catch (Exception stopError)
            {
                LaunchLog.WriteException("shutdown: force media core stop failed", stopError);
            }
        }
        finally
        {
            _allowWindowClose = true;
            try
            {
                _appWindow.Closing -= OnAppWindowClosing;
            }
            catch (Exception ex)
            {
                LaunchLog.WriteException("shutdown: detach closing handler", ex);
            }

            try
            {
                Close();
                if (cleanupSucceeded)
                {
                    // Normal shutdown stays on the UI thread and lets WinUI
                    // leave its event loop. The watchdog is only a last resort
                    // if native background resources keep the process alive.
                    Application.Current.Exit();
                }
            }
            catch (Exception ex)
            {
                cleanupSucceeded = false;
                LaunchLog.WriteException("shutdown: Close failed", ex);
            }

            if (!cleanupSucceeded) ApplicationLifecycle.ForceExit();
        }
    }

    private static void TryShutdownStep(string name, Action action)
    {
        try { action(); }
        catch (Exception ex) { LaunchLog.WriteException($"shutdown: {name} failed", ex); }
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        TryShutdownStep("resource monitor", StopResourceMonitoring);
        TryShutdownStep("window chrome", () => WindowChromeService.ClearScheduledReapply(this));
        App.NotifyMainWindowClosed();
    }
}
