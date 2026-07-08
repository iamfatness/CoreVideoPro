using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Services;

/// <summary>
/// Applies dark title-bar chrome and DWM caption colors so min/max/close are visible on launch.
/// </summary>
public static class WindowChromeService
{
    private const int DwmwaUseImmersiveDarkModeLegacy = 19;
    private const int DwmwaUseImmersiveDarkMode = 20;
    private const int DwmwaCaptionColor = 35;
    private const int DwmwaTextColor = 36;
    private const int DwmwaBorderColor = 34;

    private static readonly Windows.UI.Color TitleBackground = Windows.UI.Color.FromArgb(255, 10, 15, 22);
    private static readonly Windows.UI.Color TitleForeground = Windows.UI.Color.FromArgb(255, 232, 240, 236);
    private static readonly Windows.UI.Color TitleButtonBackground = Windows.UI.Color.FromArgb(255, 22, 30, 38);
    private static readonly Windows.UI.Color TitleButtonHover = Windows.UI.Color.FromArgb(255, 38, 52, 64);
    private static readonly Windows.UI.Color TitleButtonPressed = Windows.UI.Color.FromArgb(255, 52, 72, 88);
    private static readonly Windows.UI.Color TitleInactiveForeground = Windows.UI.Color.FromArgb(255, 148, 165, 155);

    private static readonly ConcurrentDictionary<IntPtr, List<DispatcherQueueTimer>> ScheduledTimers = new();
    private static readonly ConcurrentDictionary<IntPtr, byte> ApplyInProgress = new();

    // The CoreVideo Pro brand mark, resolved once next to the exe.
    private static readonly string AppIconPath =
        Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico");

    // extendTitleBar: the main window merges its branded top bar into the title
    // bar (no separate Windows caption strip with a duplicate "CoreVideo Pro");
    // pop-outs keep the standard caption.
    public static void Apply(Window window, AppWindow appWindow, bool extendTitleBar = false)
    {
        TrySetIcon(window, appWindow);
        ApplyCore(window, appWindow, extendTitleBar);
        ApplyCore(window, appWindow, extendTitleBar);

        // WinUI often paints default white caption buttons until after the first frame.
        var queue = DispatcherQueue.GetForCurrentThread();
        if (queue is not null)
        {
            var hwnd = WindowNative.GetWindowHandle(window);
            ScheduleReapply(queue, hwnd, window, appWindow, TimeSpan.FromMilliseconds(50), extendTitleBar);
            ScheduleReapply(queue, hwnd, window, appWindow, TimeSpan.FromMilliseconds(200), extendTitleBar);
            ScheduleReapply(queue, hwnd, window, appWindow, TimeSpan.FromMilliseconds(600), extendTitleBar);
        }
    }

    private const uint ImageIcon = 1;
    private const uint LrLoadFromFile = 0x00000010;
    private const uint WmSetIcon = 0x0080;
    private static readonly IntPtr IconSmall = IntPtr.Zero;
    private static readonly IntPtr IconBig = new(1);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadImage(IntPtr hinst, string name, uint type, int cx, int cy, uint fuLoad);

    [DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    // Sets the window/taskbar icon to the brand mark. AppWindow.SetIcon drives
    // the taskbar + alt-tab; a direct WM_SETICON also sets the standard caption's
    // small icon (top-left of the title bar), which SetIcon alone can leave as
    // the default. Best-effort: a missing/locked icon must never crash a window.
    private static void TrySetIcon(Window window, AppWindow appWindow)
    {
        try
        {
            if (!File.Exists(AppIconPath))
            {
                return;
            }
            appWindow.SetIcon(AppIconPath);

            var hwnd = WindowNative.GetWindowHandle(window);
            var small = LoadImage(IntPtr.Zero, AppIconPath, ImageIcon, 16, 16, LrLoadFromFile);
            var big = LoadImage(IntPtr.Zero, AppIconPath, ImageIcon, 32, 32, LrLoadFromFile);
            if (small != IntPtr.Zero)
            {
                SendMessage(hwnd, WmSetIcon, IconSmall, small);
            }
            if (big != IntPtr.Zero)
            {
                SendMessage(hwnd, WmSetIcon, IconBig, big);
            }
        }
        catch
        {
            // Icon is cosmetic; never let it take a window down.
        }
    }

    public static void ClearScheduledReapply(Window window)
    {
        var hwnd = WindowNative.GetWindowHandle(window);
        if (!ScheduledTimers.TryRemove(hwnd, out var timers))
        {
            return;
        }

        foreach (var timer in timers)
        {
            timer.Stop();
        }
    }

    private static void ScheduleReapply(
        DispatcherQueue queue,
        IntPtr hwnd,
        Window window,
        AppWindow appWindow,
        TimeSpan delay,
        bool extendTitleBar)
    {
        var timer = queue.CreateTimer();
        timer.Interval = delay;
        timer.IsRepeating = false;

        void OnTick(DispatcherQueueTimer sender, object args)
        {
            sender.Stop();
            sender.Tick -= OnTick;
            RemoveTimer(hwnd, sender);
            ApplyCore(window, appWindow, extendTitleBar);
        }

        timer.Tick += OnTick;
        TrackTimer(hwnd, timer);
        timer.Start();
    }

    private static void TrackTimer(IntPtr hwnd, DispatcherQueueTimer timer)
    {
        var timers = ScheduledTimers.GetOrAdd(hwnd, _ => []);
        lock (timers)
        {
            timers.Add(timer);
        }
    }

    private static void RemoveTimer(IntPtr hwnd, DispatcherQueueTimer timer)
    {
        if (!ScheduledTimers.TryGetValue(hwnd, out var timers))
        {
            return;
        }

        lock (timers)
        {
            timers.Remove(timer);
            if (timers.Count == 0)
            {
                ScheduledTimers.TryRemove(hwnd, out _);
            }
        }
    }

    private static void ApplyCore(Window window, AppWindow appWindow, bool extendTitleBar)
    {
        var hwnd = WindowNative.GetWindowHandle(window);
        if (hwnd == IntPtr.Zero)
        {
            return;
        }

        if (!ApplyInProgress.TryAdd(hwnd, 0))
        {
            return;
        }

        try
        {
            EnableImmersiveDarkMode(hwnd);
            // When the app draws into the title bar there is no system caption
            // strip to colour; setting DWM caption colours would force one back.
            if (!extendTitleBar)
            {
                ApplyDwmCaptionColors(hwnd);
            }
            ApplyAppWindowTitleBar(appWindow, extendTitleBar);
        }
        catch (Exception ex) when (ex is ArgumentException or COMException or ObjectDisposedException)
        {
            LaunchLog.Write($"chrome: skipped apply ({ex.GetType().Name}: {ex.Message})");
        }
        finally
        {
            ApplyInProgress.TryRemove(hwnd, out _);
        }
    }

    private static void ApplyAppWindowTitleBar(AppWindow appWindow, bool extendTitleBar)
    {
        if (appWindow.TitleBar is null)
        {
            return;
        }

        var background = TitleBackground;
        var foreground = TitleForeground;
        var buttonHover = TitleButtonHover;
        var buttonPressed = TitleButtonPressed;
        var inactiveForeground = TitleInactiveForeground;
        var transparent = Windows.UI.Color.FromArgb(0, 0, 0, 0);

        // NOTE: do NOT set appWindow.TitleBar.ExtendsContentIntoTitleBar here.
        // The main window extends via the Window API (Window.ExtendsContentIntoTitleBar
        // + SetTitleBar); poking the AppWindow flag on the reapply timers switches
        // the title bar into a conflicting mode and re-shows the caption.

        if (extendTitleBar)
        {
            // App draws its own top bar: only the caption BUTTONS remain, sitting
            // ON the app surface. Set ONLY their colours (transparent bg so they
            // blend); touching the caption Background/Foreground would make WinUI
            // render a title strip again.
            appWindow.TitleBar.ButtonBackgroundColor = transparent;
            appWindow.TitleBar.ButtonInactiveBackgroundColor = transparent;
            appWindow.TitleBar.ButtonForegroundColor = foreground;
            appWindow.TitleBar.ButtonInactiveForegroundColor = inactiveForeground;
            appWindow.TitleBar.ButtonHoverBackgroundColor = buttonHover;
            appWindow.TitleBar.ButtonPressedBackgroundColor = buttonPressed;
            return;
        }

        appWindow.TitleBar.BackgroundColor = background;
        appWindow.TitleBar.ForegroundColor = foreground;
        appWindow.TitleBar.InactiveBackgroundColor = background;
        appWindow.TitleBar.InactiveForegroundColor = inactiveForeground;
        appWindow.TitleBar.ButtonBackgroundColor = TitleButtonBackground;
        appWindow.TitleBar.ButtonForegroundColor = foreground;
        appWindow.TitleBar.ButtonHoverBackgroundColor = buttonHover;
        appWindow.TitleBar.ButtonPressedBackgroundColor = buttonPressed;
        appWindow.TitleBar.ButtonInactiveBackgroundColor = TitleButtonBackground;
        appWindow.TitleBar.ButtonInactiveForegroundColor = inactiveForeground;
    }

    private static void EnableImmersiveDarkMode(IntPtr hwnd)
    {
        var dark = 1;
        _ = DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkModeLegacy, ref dark, sizeof(int));
        _ = DwmSetWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, ref dark, sizeof(int));
    }

    private static void ApplyDwmCaptionColors(IntPtr hwnd)
    {
        var caption = ToColorRef(TitleBackground);
        var text = ToColorRef(TitleForeground);
        var border = ToColorRef(Windows.UI.Color.FromArgb(255, 42, 52, 60));
        _ = DwmSetWindowAttribute(hwnd, DwmwaCaptionColor, ref caption, sizeof(int));
        _ = DwmSetWindowAttribute(hwnd, DwmwaTextColor, ref text, sizeof(int));
        _ = DwmSetWindowAttribute(hwnd, DwmwaBorderColor, ref border, sizeof(int));
    }

    private static int ToColorRef(Windows.UI.Color color) =>
        color.R | (color.G << 8) | (color.B << 16);

    [DllImport("dwmapi.dll", PreserveSig = true)]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);
}