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
            "multiviewer" or "multiview" => MultiviewerPanel,
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

    private void OnMultiviewerClicked(object sender, RoutedEventArgs args) => ShowSection("multiviewer");

    private void OnLicenseClicked(object sender, RoutedEventArgs args) => ShowSection("license");

    private void OnFfmpegClicked(object sender, RoutedEventArgs args) => ShowSection("ffmpeg");

    // Pick the folder where recordings are written. The whole path plumbing already
    // exists (RecordingTargetFolder -> targetFolder wire -> core resolveTargetDir); this
    // just gives the operator a real folder picker instead of hand-typing a path.
    private async void OnBrowseRecordingFolderClicked(object sender, RoutedEventArgs args)
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker
            {
                SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.VideosLibrary,
            };
            picker.FileTypeFilter.Add("*"); // required or PickSingleFolderAsync throws
            InitializeWithWindow.Initialize(picker, WindowNative.GetWindowHandle(this));
            var folder = await picker.PickSingleFolderAsync();
            if (folder is not null && !string.IsNullOrWhiteSpace(folder.Path))
            {
                ViewModel.RecordingTargetFolder = folder.Path;
            }
        }
        catch
        {
            // Picker can throw if the shell COM apartment is busy; leave the field as-is.
        }
    }

    // Reveal the current recording folder in Explorer so the operator can confirm where
    // files land (creates it if it does not exist yet).
    private void OnOpenRecordingFolderClicked(object sender, RoutedEventArgs args)
    {
        var path = ViewModel.RecordingTargetFolder;
        if (string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        try
        {
            System.IO.Directory.CreateDirectory(path);
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
            {
                FileName = path,
                UseShellExecute = true,
            });
        }
        catch
        {
            // Non-fatal: an invalid/inaccessible path just doesn't open.
        }
    }

    private void ShowPanel(FrameworkElement activePanel)
    {
        OutputPanel.Visibility = activePanel == OutputPanel ? Visibility.Visible : Visibility.Collapsed;
        StreamingPanel.Visibility = activePanel == StreamingPanel ? Visibility.Visible : Visibility.Collapsed;
        AudioPanel.Visibility = activePanel == AudioPanel ? Visibility.Visible : Visibility.Collapsed;
        RecordingPanel.Visibility = activePanel == RecordingPanel ? Visibility.Visible : Visibility.Collapsed;
        MultiviewerPanel.Visibility = activePanel == MultiviewerPanel ? Visibility.Visible : Visibility.Collapsed;
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
