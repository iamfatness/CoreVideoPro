using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class ProductionSettingsWindow : Window
{
    private const int WindowWidth = 1180;
    private const int WindowHeight = 780;

    public ProductionSettingsWindow(StudioViewModel viewModel)
    {
        ViewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        InitializeComponent();
        Closed += OnWindowClosed;
        ApplyChromeAndSize();
    }

    public StudioViewModel ViewModel { get; }

    public event EventHandler? WindowClosed;

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        Closed -= OnWindowClosed;
        WindowClosed?.Invoke(this, EventArgs.Empty);
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
