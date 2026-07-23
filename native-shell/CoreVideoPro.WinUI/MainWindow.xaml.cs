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

    public StudioViewModel ViewModel { get; }

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

            var lan = string.Equals(Environment.GetEnvironmentVariable("COREVIDEO_OSC_LAN"), "1", StringComparison.Ordinal);

            _controlSurface = new StudioControlSurface(ViewModel, _dispatcher);

            _controlServer = new OscControlServer(_controlSurface, new OscControlServerOptions
            {
                ListenPort = port,
                BindAddress = lan ? IPAddress.Any : IPAddress.Loopback
            });
            _controlServer.Start();
            LaunchLog.Write($"control: OSC server listening on {(lan ? "0.0.0.0" : "127.0.0.1")}:{_controlServer.BoundPort}");

            // HTTP + WebSocket API sharing the same surface. Loopback needs no privileges;
            // LAN ("+") may require a Windows urlacl. Optional bearer token for LAN safety.
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
                    Host = lan ? "+" : "127.0.0.1",
                    AuthToken = Environment.GetEnvironmentVariable("COREVIDEO_CONTROL_TOKEN")
                });
                _httpControlServer.Start();
                LaunchLog.Write($"control: HTTP/WS API listening on http://{(lan ? "+" : "127.0.0.1")}:{httpPort}/ (GET /manifest, /state, /ws; POST /invoke)");
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
        if (_httpControlServer is not null)
        {
            try
            {
                await _httpControlServer.DisposeAsync().ConfigureAwait(false);
            }
            catch
            {
                // best effort
            }

            _httpControlServer = null;
        }

        if (_controlServer is not null)
        {
            try
            {
                await _controlServer.DisposeAsync().ConfigureAwait(false);
            }
            catch
            {
                // best effort
            }

            _controlServer = null;
        }

        _controlSurface?.Dispose();
        _controlSurface = null;
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

        ApplicationLifecycle.PrepareShutdown();
        using var watchdog = new System.Threading.Timer(
            _ => ApplicationLifecycle.ForceExit(),
            null,
            ShutdownWatchdog,
            Timeout.InfiniteTimeSpan);

        try
        {
            StopResourceMonitoring();
            await StopControlServerAsync().ConfigureAwait(false);
            WindowChromeService.ClearScheduledReapply(this);
            RootContent.ViewModel = null;
            await Task.Run(async () =>
                {
                    await ViewModel.DisposeAsync().ConfigureAwait(false);
                })
                .WaitAsync(ShutdownTimeout)
                .ConfigureAwait(false);
            LaunchLog.Write("shutdown: resources released");
        }
        catch (TimeoutException)
        {
            LaunchLog.Write("shutdown: dispose timed out — forcing media core stop");
            try
            {
                await ViewModel.ForceStopMediaCoreAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                LaunchLog.Write($"shutdown: force media core stop failed ({ex.Message})");
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"shutdown: dispose failed ({ex.Message})");
        }
        finally
        {
            _allowWindowClose = true;
            try
            {
                _appWindow.Closing -= OnAppWindowClosing;
            }
            catch
            {
                // Best effort.
            }

            try
            {
                Close();
            }
            catch (Exception ex)
            {
                LaunchLog.Write($"shutdown: Close() failed ({ex.Message})");
            }

            ApplicationLifecycle.ForceExit();
        }
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        StopResourceMonitoring();
        WindowChromeService.ClearScheduledReapply(this);
        App.NotifyMainWindowClosed();
    }
}