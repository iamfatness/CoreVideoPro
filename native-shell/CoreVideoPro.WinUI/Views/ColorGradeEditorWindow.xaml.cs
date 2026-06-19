using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace CoreVideoPro.WinUI.Views;

/// <summary>
/// Small pop-out window for editing a single source's color grade. UI-only this round:
/// the grade is handed back to the host VM via the editor VM's GradeSaved event.
/// </summary>
public sealed partial class ColorGradeEditorWindow : Window
{
    private const int WindowWidth = 400;
    private const int WindowHeight = 520;

    public ColorGradeEditorWindow(ColorGradeEditorViewModel viewModel)
    {
        ViewModel = viewModel ?? throw new ArgumentNullException(nameof(viewModel));
        InitializeComponent();

        ViewModel.GradeSaved += OnGradeSaved;
        ViewModel.Closed += OnEditorClosed;
        Closed += OnWindowClosed;

        ApplyChromeAndSize();
    }

    /// <summary>Mirrors how pages expose their bound view-model.</summary>
    public ColorGradeEditorViewModel ViewModel { get; }

    private void OnGradeSaved(object? sender, ColorGrade grade) => Close();

    private void OnEditorClosed(object? sender, EventArgs e) => Close();

    private void OnWindowClosed(object sender, WindowEventArgs args)
    {
        ViewModel.GradeSaved -= OnGradeSaved;
        ViewModel.Closed -= OnEditorClosed;
        Closed -= OnWindowClosed;
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
