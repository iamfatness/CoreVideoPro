using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI;

public sealed partial class MainWindow : Window
{
    private static readonly TimeSpan ShutdownTimeout = TimeSpan.FromSeconds(8);

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

        ConfigureWindowChrome();
        _appWindow.Closing += OnAppWindowClosing;
        Closed += OnWindowClosed;
    }

    private void ConfigureWindowChrome()
    {
        Title = "CoreVideo Pro";
        _appWindow.Show();
        _appWindow.Resize(new Windows.Graphics.SizeInt32(1600, 960));

        if (_appWindow.TitleBar is null)
        {
            return;
        }

        var background = ColorHelper.FromArgb(255, 10, 15, 22);
        var foreground = ColorHelper.FromArgb(255, 232, 240, 236);
        var buttonHover = ColorHelper.FromArgb(255, 30, 42, 52);
        var buttonPressed = ColorHelper.FromArgb(255, 42, 58, 70);
        var buttonInactive = ColorHelper.FromArgb(255, 18, 24, 32);

        _appWindow.TitleBar.ExtendsContentIntoTitleBar = false;
        _appWindow.TitleBar.BackgroundColor = background;
        _appWindow.TitleBar.ForegroundColor = foreground;
        _appWindow.TitleBar.InactiveBackgroundColor = background;
        _appWindow.TitleBar.InactiveForegroundColor = ColorHelper.FromArgb(255, 148, 165, 155);
        _appWindow.TitleBar.ButtonBackgroundColor = Colors.Transparent;
        _appWindow.TitleBar.ButtonForegroundColor = foreground;
        _appWindow.TitleBar.ButtonHoverBackgroundColor = buttonHover;
        _appWindow.TitleBar.ButtonPressedBackgroundColor = buttonPressed;
        _appWindow.TitleBar.ButtonInactiveBackgroundColor = buttonInactive;
        _appWindow.TitleBar.ButtonInactiveForegroundColor = ColorHelper.FromArgb(255, 148, 165, 155);
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
            LaunchLog.Write("shutdown: close requested while cleanup is in progress");
            return;
        }

        _shutdownStarted = true;
        LaunchLog.Write("shutdown: close requested");
        _ = ShutdownAsync();
    }

    private async Task ShutdownAsync()
    {
        ApplicationLifecycle.PrepareShutdown();

        try
        {
            RootContent.ViewModel = null;
            await ViewModel.DisposeAsync()
                .AsTask()
                .WaitAsync(ShutdownTimeout)
                .ConfigureAwait(true);
            LaunchLog.Write("shutdown: resources released");
        }
        catch (TimeoutException)
        {
            LaunchLog.Write("shutdown: dispose timed out");
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