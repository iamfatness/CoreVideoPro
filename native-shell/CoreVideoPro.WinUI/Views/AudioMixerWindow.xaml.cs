using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Pop-out audio mixer window. B5: hosts the SAME AudioPage console the Audio
/// tab renders (one implementation, no forked strip layouts); this class owns
/// only window chrome and lifetime.
/// </summary>
public sealed partial class AudioMixerWindow : Window
{
    private const int WindowWidth = 1080;
    private const int WindowHeight = 700;
    private bool _chromeApplied;

    public AudioMixerWindow(StudioViewModel viewModel)
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
        catch (Exception ex)
        {
            ViewModel.ReportAudioMixerFailure(ex);
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
