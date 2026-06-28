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
    private static int _firstChanceWrongThread;

    public App()
    {
        InitializeComponent();
        ApplicationLifecycle.BindActivation(Activation);
        // DIAGNOSTIC (limited to 5 writes so it can't destabilize): the recurring
        // CoreMessagingXP 0xc000027b crash is a cross-thread UI access surfacing as
        // RPC_E_WRONG_THREAD before the native fail-fast (no managed/createdump stack).
        // First-chance fires BEFORE the fail-fast — log the stack to find the off-thread
        // binding/UI call, like we did for ApplyLiveProductionPatch.
        AppDomain.CurrentDomain.FirstChanceException += (_, e) =>
        {
            if (unchecked((uint)e.Exception.HResult) == 0x8001010Eu &&
                System.Threading.Interlocked.Increment(ref _firstChanceWrongThread) <= 5)
            {
                try { LaunchLog.Write($"firstchance RPC_E_WRONG_THREAD #{_firstChanceWrongThread}:\n{e.Exception.StackTrace}"); } catch { }
            }
        };
        UnhandledException += (_, e) =>
        {
            LaunchLog.Write($"unhandled: {e.Exception}");
            // Safety net: if an x:Bind update is ever still raised off the UI thread
            // it surfaces as COMException RPC_E_WRONG_THREAD (0x8001010E). The bound
            // value re-applies on the next UI-thread update, so mark it handled and
            // keep the operator app alive instead of fail-fasting the whole process.
            if (e.Exception is System.Runtime.InteropServices.COMException com &&
                unchecked((uint)com.HResult) == 0x8001010Eu)
            {
                e.Handled = true;
            }
        };
    }

    internal static void NotifyMainWindowClosed() => _window = null;

    internal static IntPtr MainWindowHandle =>
        _window is null ? IntPtr.Zero : WindowNative.GetWindowHandle(_window);

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

            var mainWindow = new MainWindow();
            _window = mainWindow;
            ApplyWindowChromeBeforeShow(mainWindow);
            mainWindow.Activate();
            mainWindow.ViewModel.HandleAppActivation(Activation.CurrentActivationArguments);

            BringMainWindowToForeground(mainWindow);
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

    private static void ApplyWindowChromeBeforeShow(MainWindow window)
    {
        var hwnd = WindowNative.GetWindowHandle(window);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        if (appWindow is not null)
        {
            WindowChromeService.Apply(window, appWindow);
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

        if (window is MainWindow mainWindow)
        {
            WindowChromeService.Apply(mainWindow, appWindow);
        }
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
