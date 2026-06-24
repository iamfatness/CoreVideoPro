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
        ShowSection("output");
    }

    public StudioViewModel ViewModel { get; }

    public event EventHandler? WindowClosed;

    public void ShowSection(string? section)
    {
        var normalized = string.IsNullOrWhiteSpace(section)
            ? "output"
            : section.Trim().ToLowerInvariant();

        ShowPanel(normalized switch
        {
            "stream" or "streaming" => StreamingPanel,
            "audio" => AudioPanel,
            "record" or "recording" => RecordingPanel,
            "license" or "plan" => LicensePanel,
            "ffmpeg" => FfmpegPanel,
            _ => OutputPanel
        });
    }

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        Closed -= OnWindowClosed;
        WindowClosed?.Invoke(this, EventArgs.Empty);
    }

    private void OnOutputClicked(object sender, RoutedEventArgs args) => ShowSection("output");

    private void OnStreamingClicked(object sender, RoutedEventArgs args) => ShowSection("streaming");

    private void OnAudioClicked(object sender, RoutedEventArgs args) => ShowSection("audio");

    private void OnRecordingClicked(object sender, RoutedEventArgs args) => ShowSection("recording");

    private void OnLicenseClicked(object sender, RoutedEventArgs args) => ShowSection("license");

    private void OnFfmpegClicked(object sender, RoutedEventArgs args) => ShowSection("ffmpeg");

    private void ShowPanel(FrameworkElement activePanel)
    {
        OutputPanel.Visibility = activePanel == OutputPanel ? Visibility.Visible : Visibility.Collapsed;
        StreamingPanel.Visibility = activePanel == StreamingPanel ? Visibility.Visible : Visibility.Collapsed;
        AudioPanel.Visibility = activePanel == AudioPanel ? Visibility.Visible : Visibility.Collapsed;
        RecordingPanel.Visibility = activePanel == RecordingPanel ? Visibility.Visible : Visibility.Collapsed;
        LicensePanel.Visibility = activePanel == LicensePanel ? Visibility.Visible : Visibility.Collapsed;
        FfmpegPanel.Visibility = activePanel == FfmpegPanel ? Visibility.Visible : Visibility.Collapsed;
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
