using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Pop-out dedicated PROGRAM + PREVIEW window. Presents the core's program/preview shared textures
/// (separate from the multiview wall, so both can present at once). Drag it to a second display for
/// a clean large gallery alongside the multiviewer.
/// </summary>
public sealed partial class ProgramPreviewPopoutWindow : Window
{
    private const int WindowWidth = 1280;
    private const int WindowHeight = 520;
    private bool _chromeApplied;

    public ProgramPreviewPopoutWindow(StudioViewModel viewModel)
    {
        ViewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        InitializeComponent();
        Closed += OnWindowClosed;
        Activated += OnWindowActivated;
    }

    public StudioViewModel ViewModel { get; }

    public event EventHandler? WindowClosed;

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        Closed -= OnWindowClosed;
        Activated -= OnWindowActivated;
        WindowClosed?.Invoke(this, EventArgs.Empty);
    }

    private void OnWindowActivated(object sender, WindowActivatedEventArgs args)
    {
        if (_chromeApplied)
        {
            return;
        }

        _chromeApplied = true;
        try
        {
            ApplyChromeAndSize();
        }
        catch
        {
            // Chrome/sizing is best-effort; the window still functions.
        }
    }

    private void ApplyChromeAndSize()
    {
        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        if (appWindow is null)
        {
            return;
        }

        appWindow.Resize(new Windows.Graphics.SizeInt32(WindowWidth, WindowHeight));
        WindowChromeService.Apply(this, appWindow);

        if (appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsMaximizable = true;
            presenter.IsMinimizable = true;
            presenter.IsResizable = true;
        }
    }
}
