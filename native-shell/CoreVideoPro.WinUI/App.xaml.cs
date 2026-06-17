using CoreVideoPro.WinUI.Services;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;
using WinRT.Interop;

namespace CoreVideoPro.WinUI;

public partial class App : Application
{
    private static readonly AppActivationCoordinator Activation = new();
    private static Window? _window;

    public App()
    {
        InitializeComponent();
        ApplicationLifecycle.BindActivation(Activation);
        UnhandledException += (_, e) =>
        {
            LaunchLog.Write($"unhandled: {e.Exception}");
        };
    }

    internal static void NotifyMainWindowClosed() => _window = null;

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            if (await Activation.TryRedirectToPrimaryAsync().ConfigureAwait(true))
            {
                Exit();
                return;
            }

            Activation.Subscribe();
            Activation.SetActivationHandler(OnAppActivated);

            var callback = AppActivationCoordinator.FindOAuthCallbackUrl(Environment.GetCommandLineArgs());
            if (!string.IsNullOrWhiteSpace(callback))
            {
                LaunchLog.Write("oauth: startup callback argument detected");
            }

            _window = new MainWindow();
            _window.Activate();
            if (_window is MainWindow mainWindow)
            {
                mainWindow.ViewModel.HandleAppActivation(Activation.CurrentActivationArguments);
            }

            BringMainWindowToForeground(_window);
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"launch failed: {ex}");
            throw;
        }
    }

    private void OnAppActivated(AppActivationArguments args)
    {
        if (_window?.Content is not Views.StudioWorkspace workspace)
        {
            LaunchLog.Write("activation: workspace not ready");
            return;
        }

        workspace.ViewModel?.HandleAppActivation(args);
        if (_window is not null)
        {
            BringMainWindowToForeground(_window);
        }
    }

    private static void BringMainWindowToForeground(Window window)
    {
        var hwnd = WindowNative.GetWindowHandle(window);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        if (appWindow is null)
        {
            return;
        }

        appWindow.Show();
        var displayArea = DisplayArea.GetFromWindowId(windowId, DisplayAreaFallback.Primary);
        if (displayArea is not null)
        {
            var work = displayArea.WorkArea;
            const int width = 1440;
            const int height = 900;
            var x = work.X + Math.Max(0, (work.Width - width) / 2);
            var y = work.Y + Math.Max(0, (work.Height - height) / 2);
            appWindow.MoveAndResize(new Windows.Graphics.RectInt32(x, y, width, height));
        }

        if (appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsMaximizable = true;
            presenter.IsMinimizable = true;
            presenter.IsResizable = true;
            presenter.Restore();
        }

        NativeWindow.ShowAndForeground(hwnd);
        window.Activate();
    }

    private static class NativeWindow
    {
        [System.Runtime.InteropServices.DllImport("user32.dll")]
        private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

        [System.Runtime.InteropServices.DllImport("user32.dll")]
        private static extern bool SetForegroundWindow(IntPtr hWnd);

        internal static void ShowAndForeground(IntPtr hwnd)
        {
            if (hwnd == IntPtr.Zero)
            {
                return;
            }

            ShowWindow(hwnd, 9);
            SetForegroundWindow(hwnd);
        }
    }
}