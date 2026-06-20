using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using System.ComponentModel;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Pop-out window for editing a single source's color grade with a live preview.
/// </summary>
public sealed partial class ColorGradeEditorWindow : Window
{
    private const int WindowWidth = 920;
    private const int WindowHeight = 640;

    public ColorGradeEditorWindow(ColorGradeEditorViewModel viewModel)
    {
        ViewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        InitializeComponent();

        ViewModel.GradeSaved += OnGradeSaved;
        ViewModel.Closed += OnEditorClosed;
        ViewModel.PropertyChanged += OnViewModelPropertyChanged;
        Closed += OnWindowClosed;

        ApplyChromeAndSize();
        RefreshPreviewImage();
    }

    /// <summary>Mirrors how pages expose their bound view-model.</summary>
    public ColorGradeEditorViewModel ViewModel { get; }

    private void OnGradeSaved(object? sender, ColorGrade grade) => Close();

    private void OnEditorClosed(object? sender, EventArgs e) => Close();

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        ViewModel.GradeSaved -= OnGradeSaved;
        ViewModel.Closed -= OnEditorClosed;
        ViewModel.PropertyChanged -= OnViewModelPropertyChanged;
        Closed -= OnWindowClosed;
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(ColorGradeEditorViewModel.GradedPreviewBgra) or
            nameof(ColorGradeEditorViewModel.GradedPreviewWidth) or
            nameof(ColorGradeEditorViewModel.GradedPreviewHeight) or
            nameof(ColorGradeEditorViewModel.HasPreview))
        {
            RefreshPreviewImage();
        }
    }

    private void RefreshPreviewImage()
    {
        if (ViewModel.HasPreview)
        {
            BgraPreviewHelper.SetPreview(
                GradedPreviewImage,
                ViewModel.GradedPreviewBgra,
                ViewModel.GradedPreviewWidth,
                ViewModel.GradedPreviewHeight);
            PreviewPlaceholder.Visibility = Visibility.Collapsed;
            return;
        }

        BgraPreviewHelper.ClearPreview(GradedPreviewImage);
        PreviewPlaceholder.Visibility = Visibility.Visible;
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
            presenter.IsMaximizable = false;
            presenter.IsMinimizable = false;
            presenter.IsResizable = true;
        }
    }
}
