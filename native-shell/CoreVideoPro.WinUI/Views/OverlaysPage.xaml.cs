using System.ComponentModel;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class OverlaysPage : UserControl
{
    private StudioViewModel? _boundViewModel;

    public OverlaysPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { Bind(ViewModel); ApplyGraphicsMode(); };
        Unloaded += (_, _) => Bind(null);
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
            new PropertyMetadata(null, (d, e) => { var page = (OverlaysPage)d; if (page.IsLoaded) { page.Bind((StudioViewModel?)e.NewValue); page.ApplyGraphicsMode(); } }));

    // ---- #1076 Playout | Design -------------------------------------------------------------
    private void Bind(StudioViewModel? next)
    {
        if (ReferenceEquals(_boundViewModel, next)) return;
        if (_boundViewModel is not null) _boundViewModel.PropertyChanged -= OnViewModelPropertyChanged;
        _boundViewModel = next;
        if (_boundViewModel is not null) _boundViewModel.PropertyChanged += OnViewModelPropertyChanged;
    }
    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.GraphicsMode) or nameof(StudioViewModel.GraphicsDesignAdvanced)) ApplyGraphicsMode();
    }
    private void OnPlayoutModeClick(object sender, RoutedEventArgs e) { if (ViewModel is { } vm) vm.GraphicsMode = StudioViewModel.GraphicsModePlayout; }
    private void OnDesignModeClick(object sender, RoutedEventArgs e) { if (ViewModel is { } vm) vm.GraphicsMode = StudioViewModel.GraphicsModeDesign; }

    /// <summary>Segmented-toggle chrome plus the workspace preview lease: the Design canvas only
    /// holds its private preview while it is the visible mode.</summary>
    private void ApplyGraphicsMode()
    {
        var design = ViewModel?.IsGraphicsDesignMode == true;
        var accent = (Brush)Application.Current.Resources["StudioAccentBrush"];
        var text = (Brush)Application.Current.Resources["StudioTextBrush"];
        var onAccent = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 0x06, 0x17, 0x0D));
        var clear = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        PlayoutModeButton.Background = design ? clear : accent; PlayoutModeButton.Foreground = design ? text : onAccent;
        DesignModeButton.Background = design ? accent : clear; DesignModeButton.Foreground = design ? onAccent : text;
        if (design) ResumeWorkspacePreviewIfDesign(); else { GraphicsWorkspace.SuspendPreview(); GraphicsBuilder.SuspendPreview(); }
    }
    /// <summary>Exactly one Design surface holds the private preview: the builder by default,
    /// the advanced workspace behind "Advanced editor".</summary>
    private void ResumeWorkspacePreviewIfDesign()
    {
        if (ViewModel?.IsGraphicsDesignMode != true) return;
        if (LegacyDesignerExpander.IsExpanded && ExistingOverlayDrawer.IsPaneOpen) return;
        if (ViewModel.GraphicsDesignAdvanced) { GraphicsBuilder.SuspendPreview(); GraphicsWorkspace.ResumePreview(); }
        else { GraphicsWorkspace.SuspendPreview(); GraphicsBuilder.ResumePreview(); }
    }

    private void OnExistingControlsRequested(object? sender, EventArgs e) {
        ExistingOverlayDrawer.IsPaneOpen = true;
        if (LegacyDesignerExpander.IsExpanded) { GraphicsWorkspace.SuspendPreview(); GraphicsBuilder.SuspendPreview(); LowerThirdDesigner.ActivatePreview(); }
    }
    private void OnCloseExistingControls(object sender, RoutedEventArgs e) {
        ExistingOverlayDrawer.IsPaneOpen = false;
        LowerThirdDesigner.DeactivatePreview(); ResumeWorkspacePreviewIfDesign();
    }
    private void OnExistingPaneClosed(SplitView sender, object args) {
        LowerThirdDesigner.DeactivatePreview(); ResumeWorkspacePreviewIfDesign();
    }

    private void OnDesignerExpanding(Expander sender,ExpanderExpandingEventArgs args) { GraphicsWorkspace.SuspendPreview(); LowerThirdDesigner.ActivatePreview(); }
    private void OnDesignerCollapsed(Expander sender,ExpanderCollapsedEventArgs args) { LowerThirdDesigner.DeactivatePreview(); ResumeWorkspacePreviewIfDesign(); }

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
