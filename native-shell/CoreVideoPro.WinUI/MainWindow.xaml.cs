using CoreVideoPro.Control;
using CoreVideoPro.Control.Http;
using CoreVideoPro.Control.Osc;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.ShowEngine;
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
    private ShowEngineBridge? _showEngineBridge;
    // The supervisor the bridge wraps. Held HERE rather than made the bridge's property, because
    // the bridge does not own it (it is handed one in its constructor and unhooks its events on
    // Dispose, nothing more) and giving it ownership would change the disposal contract of every
    // caller that constructs the pair. It owns the child PROCESS handle, so it must be disposed on
    // the shutdown path or the show engine outlives the shell.
    private ShowEngineSupervisor? _showEngineSupervisor;
    private Action<NativeMediaCoreStateSnapshot>? _showEngineRosterPublisher;
    // Where StartShowEngine wrote the effective config, so ApplyShowConfigAsync rewrites THE SAME
    // file the running engine was spawned against. Resolved even when the engine never started
    // (no config at launch) — first-time setup still has to write somewhere.
    private string? _ohgConfigFolder;
    private string? _ohgEffectiveConfigPath;
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

            // Wired BEFORE the engine starts, and regardless of whether it does: the settings
            // section must be able to save a first-ever config on a machine with no engine.
            ViewModel.OhgApplyConfig = ApplyShowConfigAsync;

            // The OHG show engine is optional and must NEVER be able to stop the app launching:
            // StartShowEngine swallows everything into the launch log and returns the static-only
            // catalog on any failure (spec §6.4 "the app launches regardless").
            var catalog = StartShowEngine(out var ohgAdapter);

            _controlSurface = new StudioControlSurface(ViewModel, _dispatcher, _showEngineBridge, ohgAdapter);

            _controlServer = new OscControlServer(_controlSurface, new OscControlServerOptions
            {
                ListenPort = port,
                BindAddress = oscLan ? IPAddress.Any : IPAddress.Loopback
            }, catalog);
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
                }, catalog);
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

    // ---- OHG show engine startup (Plan 7a Task 11, brief's ordered paragraph) --------
    //
    // Order: config store -> Exists -> Load -> Validate against the shell's scene ids ->
    // resolve node/entry paths -> bridge over a supervisor -> host adapter over the ViewModel
    // facade -> catalog -> roster/active-speaker/capacity intake -> fire-and-forget StartAsync.
    //
    // EVERY failure is a LaunchLog line and a fall back to ControlCatalog.StaticOnly. The app
    // runs without OHG; it never fails to launch because of it.
    private ControlCatalog StartShowEngine(out OhgHostAdapter? adapter)
    {
        adapter = null;
        try
        {
            var folder = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "CoreVideoPro");
            var store = new ShowConfigStore(folder);
            _ohgConfigFolder = folder;
            _ohgEffectiveConfigPath = Path.Combine(folder, EffectiveConfigFileName);
            if (!store.Exists)
            {
                // The overwhelmingly common case: no show configured. Silent by design - this is
                // not a failure, and a line here would appear in every launch log forever.
                return ControlCatalog.StaticOnly;
            }

            var config = store.Load(out var loadError);
            if (config is null)
            {
                LaunchLog.Write($"ohg: show config could not be read ({loadError}) - show engine disabled");
                return ControlCatalog.StaticOnly;
            }

            var sceneIds = new HashSet<string>(ViewModel.Scenes.Select(scene => scene.Id), StringComparer.Ordinal);
            var validationError = ShowConfigValidator.Validate(config, sceneIds);
            if (validationError is not null)
            {
                LaunchLog.Write($"ohg: show config is invalid ({validationError}) - show engine disabled");
                return ControlCatalog.StaticOnly;
            }

            var paths = ShowEnginePaths.Resolve(
                File.Exists,
                AppContext.BaseDirectory,
                MediaCorePaths.RepoRoot,
                Environment.GetEnvironmentVariable,
                FindNodeOnPath);
            if (paths is null)
            {
                LaunchLog.Write("ohg: no packaged or dev show-engine host was found (set COREVIDEO_NODE_EXE + COREVIDEO_SHOW_ENGINE_DIR, or build show-engine) - show engine disabled");
                return ControlCatalog.StaticOnly;
            }

            // The engine reads ONE config file. Materialize the effective document (statePath
            // defaulted in) beside the source rather than mutating what the operator edits.
            var effectiveConfigPath = _ohgEffectiveConfigPath;
            File.WriteAllText(effectiveConfigPath, store.MaterializeEngineConfig(config));

            var oscExposure = string.Equals(Environment.GetEnvironmentVariable("COREVIDEO_OSC_OHG_LAN"), "1", StringComparison.Ordinal)
                ? OscExposure.Lan
                : OscExposure.LoopbackOnly;

            var supervisor = new ShowEngineSupervisor(
                new ProcessShowEngineChildFactory(),
                new ShowEngineRestartPolicy(),
                Task.Delay,
                () => DateTimeOffset.UtcNow);
            var bridge = new ShowEngineBridge(supervisor, oscExposure);
            bridge.Log += (_, line) => LaunchLog.Write($"ohg[{line.Level}]: {line.Message}");
            _showEngineBridge = bridge;
            _showEngineSupervisor = supervisor;

            adapter = new OhgHostAdapter(
                new StudioViewModelOhgFacade(ViewModel, LaunchLog.Write),
                config.Shell,
                LaunchLog.Write,
                // Seeded so the FIRST setPreview of a look resolves: the engine cues a look on
                // preview one seq BEFORE the applyLook that names its scene (Task 13).
                ShowConfigLooks.PresetsByLookId(config.Engine));

            // The workspace's OHG tab (Plan 7b). Built HERE, not in the ViewModel: whether the
            // show engine runs at all is an app-composition decision (config present + host
            // resolvable), and the page renders its setup surface while this stays null.
            AttachOhgShowViewModel(bridge, config);
            ViewModel.OhgEngineStartedAtLaunch = true;

            // Roster + active speaker ride the core's snapshot stream (spec 6.2). This handler
            // runs on the media-core READER thread; every Publish on the bridge is lock-guarded
            // and does no UI work, so it deliberately does NOT marshal.
            _showEngineRosterPublisher = snapshot =>
            {
                try
                {
                    bridge.PublishRoster(OhgParticipantMapper.Map(
                        snapshot.Participants ?? System.Array.Empty<RawParticipantEvent>()));
                    bridge.PublishActiveSpeaker(snapshot.ActiveSpeakerId);
                }
                catch (Exception ex)
                {
                    LaunchLog.Write($"ohg: roster publish failed ({ex.Message})");
                }
            };
            ViewModel.MediaCoreBridge.SnapshotChanged += _showEngineRosterPublisher;
            bridge.PublishCapacity(ViewModel.ShowInputEditors.Count);

            var request = new ShowEngineSpawnRequest(
                paths.NodeExe,
                paths.EntryScript,
                effectiveConfigPath,
                paths.WorkingDirectory,
                // Empty = inherit this process's environment; ProcessShowEngineChild merges.
                new Dictionary<string, string>(StringComparer.Ordinal));

            // NEVER blocks the launch: the engine takes seconds to handshake and a slow start
            // must not hold the window. It is still OBSERVED — an unawaited task that faults
            // would otherwise take the failure to the grave (the supervisor's own health is the
            // operator-facing signal; this line is the one in the launch log).
            _ = bridge.StartAsync(request, CancellationToken.None).ContinueWith(
                task => LaunchLog.Write($"ohg: engine start faulted: {task.Exception?.GetBaseException().Message}"),
                CancellationToken.None,
                TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);

            LaunchLog.Write($"ohg: show engine starting ({paths.Source}) node={paths.NodeExe} entry={paths.EntryScript} driveHost={config.Shell.DriveHost}");
            return new ControlCatalog(new[] { bridge });
        }
        catch (Exception ex)
        {
            LaunchLog.WriteException("ohg: show engine startup failed - running without it", ex);

            // A failure PART-WAY through leaves a bridge and possibly a roster subscription
            // behind. Running "without OHG" has to mean it: unhook and drop them, or the core's
            // snapshot stream keeps feeding a bridge nothing else references.
            if (_showEngineRosterPublisher is { } orphanedPublisher)
            {
                try { ViewModel.MediaCoreBridge.SnapshotChanged -= orphanedPublisher; }
                catch (Exception unhookError) { LaunchLog.Write($"ohg: roster unsubscribe failed ({unhookError.Message})"); }
                _showEngineRosterPublisher = null;
            }

            DetachOhgShowViewModel();
            ViewModel.OhgEngineStartedAtLaunch = false;

            try { _showEngineBridge?.Dispose(); }
            catch (Exception disposeError) { LaunchLog.Write($"ohg: bridge disposal failed ({disposeError.Message})"); }
            _showEngineBridge = null;

            try { _showEngineSupervisor?.Dispose(); }
            catch (Exception disposeError) { LaunchLog.Write($"ohg: supervisor disposal failed ({disposeError.Message})"); }
            _showEngineSupervisor = null;

            adapter = null;
            return ControlCatalog.StaticOnly;
        }
    }

    /// <summary>node.exe on PATH (spec 6.4 candidate 3). Null when it is not there, which simply
    /// drops the dev candidate from the resolver.</summary>
    private static string? FindNodeOnPath()
    {
        try
        {
            var path = Environment.GetEnvironmentVariable("PATH");
            if (string.IsNullOrEmpty(path))
            {
                return null;
            }

            foreach (var directory in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                var candidate = Path.Combine(directory.Trim(PathQuote), "node.exe");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"ohg: PATH scan for node.exe failed ({ex.Message})");
        }

        return null;
    }

    // PATH entries may be quoted on Windows.
    private const char PathQuote = '"';

    /// <summary>The document the engine is actually spawned against — the source config plus the
    /// statePath the store fills in. Written beside the operator's file, never over it.</summary>
    internal const string EffectiveConfigFileName = "ohg-show-config.effective.json";

    /// <summary>Builds (or rebuilds) the OHG workspace view model over <paramref name="bridge"/>.
    /// The looks list and shadow/drive mode come from the CONFIG, not the snapshot, so a rebuild
    /// is what makes an edited look list show up in the page's picker.</summary>
    private void AttachOhgShowViewModel(ShowEngineBridge bridge, ShowConfig config)
    {
        DetachOhgShowViewModel();

        var show = new OhgShowViewModel(
            new BridgeOhgActionInvoker(bridge),
            action => UiDispatch.Run(_dispatcher, action, "ohg-show"),
            () => bridge.Latest,
            () => bridge.Health,
            OhgSnapshotProjection.LookOptionsFromEngine(config.Engine),
            config.Shell.DriveHost);
        show.Attach(bridge);
        ViewModel.OhgShow = show;
    }

    /// <summary>Drops the workspace VM and its bridge subscriptions. Null-safe and idempotent —
    /// it is called on the startup failure path, on every rebuild, and at shutdown.</summary>
    private void DetachOhgShowViewModel()
    {
        var show = ViewModel.OhgShow;
        if (show is null)
        {
            return;
        }

        // Cleared FIRST so the page cannot re-render against a view model that is being disposed
        // (Dispose unhooks the bridge events; a snapshot in flight would otherwise land on it).
        ViewModel.OhgShow = null;
        show.Dispose();
    }

    /// <summary>
    /// Applies an edited show config (Plan 7b Task 10) — the settings section's Save. Returns null
    /// on success or the operator-facing error text; it NEVER throws, because the caller is a
    /// bound command on the settings page.
    ///
    /// <para>UI THREAD. The step ORDER lives in <see cref="OhgConfigApplySteps"/> (which carries
    /// the reasoning and the tests); this method is the switch that runs it. The only work that
    /// leaves the UI thread is the engine restart, which kills and respawns a child process.</para>
    ///
    /// <para>With no bridge — no config at launch, or the host was not resolvable — the order stops
    /// after writing the effective config and this returns null; the settings VM then shows its
    /// "restart CoreVideo Pro" message rather than claiming the show engine is live.</para>
    /// </summary>
    internal async Task<string?> ApplyShowConfigAsync(ShowConfig config)
    {
        if (config is null)
        {
            return "No show config to apply.";
        }

        var bridge = _showEngineBridge;

        try
        {
            foreach (var step in OhgConfigApplySteps.Order(bridge is not null))
            {
                switch (step)
                {
                    case OhgConfigApplySteps.Validate:
                    {
                        var sceneIds = new HashSet<string>(ViewModel.Scenes.Select(scene => scene.Id), StringComparer.Ordinal);
                        var problem = ShowConfigValidator.Validate(config, sceneIds);
                        if (problem is not null)
                        {
                            LaunchLog.Write($"ohg: show config not applied ({problem})");
                            return problem;
                        }
                        break;
                    }

                    case OhgConfigApplySteps.Materialize:
                    {
                        var folder = _ohgConfigFolder ?? Path.Combine(
                            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                            "CoreVideoPro");
                        _ohgConfigFolder = folder;
                        _ohgEffectiveConfigPath ??= Path.Combine(folder, EffectiveConfigFileName);
                        Directory.CreateDirectory(folder);
                        File.WriteAllText(_ohgEffectiveConfigPath, new ShowConfigStore(folder).MaterializeEngineConfig(config));
                        break;
                    }

                    case OhgConfigApplySteps.ReplaceAdapter:
                    {
                        // A NEW adapter, not a mutated one: the shell block and the look→scene
                        // preset map are captured at construction, and rebuilding is the only way
                        // an in-flight command can never see a half-updated adapter.
                        var adapter = new OhgHostAdapter(
                            new StudioViewModelOhgFacade(ViewModel, LaunchLog.Write),
                            config.Shell,
                            LaunchLog.Write,
                            ShowConfigLooks.PresetsByLookId(config.Engine));
                        _controlSurface?.ReplaceOhgAdapter(adapter);
                        break;
                    }

                    case OhgConfigApplySteps.RebuildPageViewModel:
                        AttachOhgShowViewModel(bridge!, config);
                        break;

                    case OhgConfigApplySteps.RestartEngine:
                        // Off the UI thread: a restart kill-trees the child and respawns it.
                        await Task.Run(() => bridge!.RestartAsync(CancellationToken.None)).ConfigureAwait(true);
                        break;
                }
            }

            LaunchLog.Write($"ohg: show config applied (engine {(bridge is null ? "not running" : "restarted")}, driveHost={config.Shell.DriveHost})");
            return null;
        }
        catch (Exception ex)
        {
            LaunchLog.WriteException("ohg: show config apply failed", ex);
            return $"Could not apply the show config: {ex.Message}";
        }
    }

    private async Task StopShowEngineAsync()
    {
        var bridge = _showEngineBridge;
        _showEngineBridge = null;
        if (bridge is null)
        {
            return;
        }

        if (_showEngineRosterPublisher is { } publisher)
        {
            TryShutdownStep("ohg roster publisher", () => ViewModel.MediaCoreBridge.SnapshotChanged -= publisher);
            _showEngineRosterPublisher = null;
        }

        // BEFORE the bridge stops: the workspace VM holds snapshot/health/log subscriptions on it,
        // and a stopping bridge still raises a final health event. Detached and dropped here, on
        // the UI thread, while the dispatcher is still alive.
        TryShutdownStep("ohg show view model", DetachOhgShowViewModel);

        try
        {
            // Off the UI thread: StopAsync sends a graceful shutdown then kills the tree.
            await Task.Run(() => bridge.StopAsync()).ConfigureAwait(true);
        }
        catch (Exception ex)
        {
            LaunchLog.WriteException("shutdown: OHG show engine stop", ex);
        }

        TryShutdownStep("ohg bridge", bridge.Dispose);

        // The supervisor LAST: it kill-trees anything StopAsync left alive and cancels the
        // lifetime token every parked recovery is waiting on. Without it a supervisor that was
        // mid-backoff at shutdown would still be holding a spawn intent.
        if (_showEngineSupervisor is { } supervisor)
        {
            _showEngineSupervisor = null;
            TryShutdownStep("ohg supervisor", supervisor.Dispose);
        }
    }

    private async Task StopControlServerAsync()
    {
        // Its feedback timer and VM subscriptions are UI-owned. Close the
        // command gate before any asynchronous socket teardown or VM disposal.
        // Disposing it FIRST also unhooks the show engine's snapshot/health/host-command
        // handlers, so nothing arrives at a ViewModel that is being torn down.
        TryShutdownStep("control surface", () => _controlSurface?.Dispose());
        _controlSurface = null;

        // Then the show engine, still BEFORE the control servers: it is a child PROCESS whose
        // teardown does a graceful shutdown then a kill-tree, so it rides Task.Run and never
        // the UI thread (CLAUDE.md, G4 teardown order).
        await StopShowEngineAsync().ConfigureAwait(true);
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
