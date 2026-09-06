using CoreVideoPro.WinUI.Services;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;
using WinRT.Interop;

namespace CoreVideoPro.WinUI;

public partial class App : Application
{
    private static readonly AppActivationCoordinator Activation = new();
    private static Window? _window;
    private readonly DispatcherQueue _dispatcher = DispatcherQueue.GetForCurrentThread()
        ?? throw new InvalidOperationException("Application requires a UI dispatcher.");
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
            var hr = unchecked((uint)e.Exception.HResult);
            if ((hr == 0x8001010Eu /*RPC_E_WRONG_THREAD*/ || hr == 0x80000013u /*RO_E_CLOSED*/ ||
                 e.Exception is System.InvalidCastException) &&
                System.Threading.Interlocked.Increment(ref _firstChanceWrongThread) <= 8)
            {
                // Environment.StackTrace = the LIVE thread stack (the off-thread caller
                // chain). e.Exception.StackTrace is incomplete at first-chance.
                try { LaunchLog.Write($"firstchance {e.Exception.GetType().Name} hr=0x{hr:X8} #{_firstChanceWrongThread}:\n{Environment.StackTrace}"); } catch { }
            }
        };
        UnhandledException += (_, e) =>
        {
            LaunchLog.Write($"unhandled: {e.Exception}");
            // Keep the operator app alive on RECOVERABLE exceptions instead of
            // fail-fasting the whole process. Observed culprits: off-thread x:Bind
            // updates (COMException RPC_E_WRONG_THREAD 0x8001010E) and async
            // [RelayCommand] failures surfacing as COM-interop InvalidCastExceptions.
            // The exception is logged above for root-causing; the failed operation
            // simply doesn't complete and re-runs on the next tick/user action.
            // Genuinely fatal conditions (OOM, stack overflow) are not in this set.
            if (e.Exception is System.Runtime.InteropServices.COMException
                or System.InvalidCastException
                or System.InvalidOperationException
                or System.ObjectDisposedException
                or System.OperationCanceledException)
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
        // AppInstance.Activated may arrive on an RPC worker. Do not even read
        // Window.Content or call the OAuth coordinator until on the UI thread.
        if (_dispatcher.HasThreadAccess)
        {
            HandleActivationOnUiThread(args);
        }
        else if (!_dispatcher.TryEnqueue(() => HandleActivationOnUiThread(args)))
        {
            LaunchLog.Write("activation: UI dispatcher unavailable during shutdown");
        }
    }

    private void HandleActivationOnUiThread(AppActivationArguments args)
    {
        if (_window is not MainWindow mainWindow || mainWindow.IsShuttingDown)
        {
            LaunchLog.Write("activation: main window unavailable or closing");
            return;
        }

        try
        {
            // Window.Content is the root Grid, not StudioWorkspace. The window
            // owns the view model regardless of the visual tree's container.
            mainWindow.ViewModel.HandleAppActivation(args);
            BringMainWindowToForeground(mainWindow);
        }
        catch (Exception ex)
        {
            LaunchLog.WriteException("activation: handling failed", ex);
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
