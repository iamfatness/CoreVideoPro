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

    public static void Apply(Window window, AppWindow appWindow)
    {
        TrySetIcon(appWindow);
        ApplyCore(window, appWindow);
        ApplyCore(window, appWindow);

        // WinUI often paints default white caption buttons until after the first frame.
        var queue = DispatcherQueue.GetForCurrentThread();
        if (queue is not null)
        {
            var hwnd = WindowNative.GetWindowHandle(window);
            ScheduleReapply(queue, hwnd, window, appWindow, TimeSpan.FromMilliseconds(50));
            ScheduleReapply(queue, hwnd, window, appWindow, TimeSpan.FromMilliseconds(200));
            ScheduleReapply(queue, hwnd, window, appWindow, TimeSpan.FromMilliseconds(600));
        }
    }

    // Sets the window/taskbar icon to the brand mark. Best-effort: a missing or
    // locked icon file must never crash a window's creation.
    private static void TrySetIcon(AppWindow appWindow)
    {
        try
        {
            if (File.Exists(AppIconPath))
            {
                appWindow.SetIcon(AppIconPath);
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
        TimeSpan delay)
    {
        var timer = queue.CreateTimer();
        timer.Interval = delay;
        timer.IsRepeating = false;

        void OnTick(DispatcherQueueTimer sender, object args)
        {
            sender.Stop();
            sender.Tick -= OnTick;
            RemoveTimer(hwnd, sender);
            ApplyCore(window, appWindow);
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

    private static void ApplyCore(Window window, AppWindow appWindow)
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
            ApplyDwmCaptionColors(hwnd);
            ApplyAppWindowTitleBar(appWindow);
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

    private static void ApplyAppWindowTitleBar(AppWindow appWindow)
    {
        if (appWindow.TitleBar is null)
        {
            return;
        }

        var background = TitleBackground;
        var foreground = TitleForeground;
        var buttonBackground = TitleButtonBackground;
        var buttonHover = TitleButtonHover;
        var buttonPressed = TitleButtonPressed;
        var inactiveForeground = TitleInactiveForeground;

        appWindow.TitleBar.ExtendsContentIntoTitleBar = false;
        appWindow.TitleBar.BackgroundColor = background;
        appWindow.TitleBar.ForegroundColor = foreground;
        appWindow.TitleBar.InactiveBackgroundColor = background;
        appWindow.TitleBar.InactiveForegroundColor = inactiveForeground;
        appWindow.TitleBar.ButtonBackgroundColor = buttonBackground;
        appWindow.TitleBar.ButtonForegroundColor = foreground;
        appWindow.TitleBar.ButtonHoverBackgroundColor = buttonHover;
        appWindow.TitleBar.ButtonPressedBackgroundColor = buttonPressed;
        appWindow.TitleBar.ButtonInactiveBackgroundColor = buttonBackground;
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