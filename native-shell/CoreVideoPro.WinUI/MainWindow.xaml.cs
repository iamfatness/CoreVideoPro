using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI;

public sealed partial class MainWindow : Window
{
    public StudioViewModel ViewModel { get; }

    public MainWindow()
    {
        ViewModel = new StudioViewModel();
        InitializeComponent();
        RootContent.ViewModel = ViewModel;
        ConfigureWindowChrome();
        Closed += OnClosed;
    }

    private void ConfigureWindowChrome()
    {
        Title = "CoreVideo Pro";
        var hwnd = WindowNative.GetWindowHandle(this);
        var appWindow = AppWindow.GetFromWindowId(Win32Interop.GetWindowIdFromWindow(hwnd));
        if (appWindow is not null)
        {
            appWindow.Resize(new Windows.Graphics.SizeInt32(1440, 900));
            if (appWindow.TitleBar is not null)
            {
                appWindow.TitleBar.ForegroundColor = Colors.White;
                appWindow.TitleBar.BackgroundColor = ColorHelper.FromArgb(255, 10, 15, 22);
                appWindow.TitleBar.InactiveBackgroundColor = ColorHelper.FromArgb(255, 10, 15, 22);
            }
        }
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        _ = ViewModel.DisposeAsync();
    }
}