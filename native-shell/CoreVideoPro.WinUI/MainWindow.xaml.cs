using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI;

public sealed partial class MainWindow : Window
{
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ShutdownWatchdog = TimeSpan.FromSeconds(6);

    private readonly AppWindow _appWindow;
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
        WindowChromeService.Apply(this, _appWindow);
        Activated += (_, _) => WindowChromeService.Apply(this, _appWindow);
        RootContent.Loaded += (_, _) => WindowChromeService.Apply(this, _appWindow);
        _appWindow.Closing += OnAppWindowClosing;
        Closed += OnWindowClosed;
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
        ApplicationLifecycle.PrepareShutdown();
        using var watchdog = new System.Threading.Timer(
            _ => ApplicationLifecycle.ForceExit(),
            null,
            ShutdownWatchdog,
            Timeout.InfiniteTimeSpan);

        try
        {
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

    private void OnWindowClosed(object sender, WindowEventArgs args) =>
        App.NotifyMainWindowClosed();
}