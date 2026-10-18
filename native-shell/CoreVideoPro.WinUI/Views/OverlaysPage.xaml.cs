using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class OverlaysPage : UserControl
{
    public OverlaysPage()
    {
        InitializeComponent();
    }

    public StudioViewModel? ViewModel
    {
        get => (StudioViewModel?)GetValue(ViewModelProperty);
        set => SetValue(ViewModelProperty, value);
    }

    public static readonly DependencyProperty ViewModelProperty =
        DependencyProperty.Register(
            nameof(ViewModel),
            typeof(StudioViewModel),
            typeof(OverlaysPage),
            new PropertyMetadata(null));
    private void OnExistingControlsRequested(object? sender, EventArgs e) {
        ExistingOverlayDrawer.IsPaneOpen = true;
        if (LegacyDesignerExpander.IsExpanded) { GraphicsWorkspace.SuspendPreview(); LowerThirdDesigner.ActivatePreview(); }
    }
    private void OnCloseExistingControls(object sender, RoutedEventArgs e) {
        ExistingOverlayDrawer.IsPaneOpen = false;
        LowerThirdDesigner.DeactivatePreview(); GraphicsWorkspace.ResumePreview();
    }
    private void OnExistingPaneClosed(SplitView sender, object args) {
        LowerThirdDesigner.DeactivatePreview(); GraphicsWorkspace.ResumePreview();
    }

    private void OnDesignerExpanding(Expander sender,ExpanderExpandingEventArgs args) { GraphicsWorkspace.SuspendPreview(); LowerThirdDesigner.ActivatePreview(); }
    private void OnDesignerCollapsed(Expander sender,ExpanderCollapsedEventArgs args) { LowerThirdDesigner.DeactivatePreview(); GraphicsWorkspace.ResumePreview(); }

    private void OnAddBrowserOverlayClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string browserId })
        {
            ViewModel?.AddBrowserOverlayCommand.Execute(browserId);
        }
    }

    private void OnReloadBrowserOverlayClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string browserId })
        {
            ViewModel?.ReloadBrowserOverlayCommand.Execute(browserId);
        }
    }

    private void OnToggleBrowserOverlayOnAirClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string browserId })
        {
            ViewModel?.ToggleBrowserOverlayOnAirCommand.Execute(browserId);
        }
    }

    private void OnToggleBrowserOverlayPreviewClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string browserId })
        {
            ViewModel?.ToggleBrowserOverlayPreviewCommand.Execute(browserId);
        }
    }
}
