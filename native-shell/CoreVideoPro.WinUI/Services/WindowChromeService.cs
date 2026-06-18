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

    public static void Apply(Window window, AppWindow appWindow)
    {
        var hwnd = WindowNative.GetWindowHandle(window);
        EnableImmersiveDarkMode(hwnd);
        ApplyDwmCaptionColors(hwnd);
        ApplyAppWindowTitleBar(appWindow);

        // WinUI often paints default white caption buttons until after the first frame.
        var queue = DispatcherQueue.GetForCurrentThread();
        if (queue is not null)
        {
            ScheduleReapply(queue, window, appWindow, TimeSpan.FromMilliseconds(50));
            ScheduleReapply(queue, window, appWindow, TimeSpan.FromMilliseconds(200));
            ScheduleReapply(queue, window, appWindow, TimeSpan.FromMilliseconds(600));
        }
    }

    private static void ScheduleReapply(
        DispatcherQueue queue,
        Window window,
        AppWindow appWindow,
        TimeSpan delay)
    {
        var timer = queue.CreateTimer();
        timer.Interval = delay;
        timer.IsRepeating = false;
        timer.Tick += (_, _) =>
        {
            timer.Stop();
            Apply(window, appWindow);
        };
        timer.Start();
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