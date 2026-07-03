using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Pop-out multiviewer window — hosts the SAME core-composited multiview shared texture as the
/// Studio view. Because that texture is single-consumer (keyed mutex), the main Studio view
/// unbinds it (StudioViewModel.MultiviewPoppedOut) while this window is open; only one presents at
/// a time. Drag this to a second display for the classic broadcast multiviewer workflow.
/// </summary>
public sealed partial class MultiviewPopoutWindow : Window
{
    private const int WindowWidth = 1280;
    private const int WindowHeight = 760;
    private bool _chromeApplied;

    public MultiviewPopoutWindow(StudioViewModel viewModel)
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

    private void OnPopInClicked(object sender, RoutedEventArgs e) => ViewModel.PopInMultiviewCommand.Execute(null);

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
