using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesInputsPage : UserControl
{
    public SourcesInputsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => AttachPreviewInterest();
        Unloaded += (_, _) => DetachPreviewInterest();
    }

    private StudioViewModel? _previewOwner;

    private void AttachPreviewInterest()
    {
        if (_previewOwner == ViewModel) return;
        DetachPreviewInterest();
        _previewOwner = ViewModel;
        if (_previewOwner is null) return;
        _previewOwner.PropertyChanged += OnPreviewOwnerChanged;
        _previewOwner.SetInputPreviewVisible(_previewOwner.IsInputsTab);
    }

    private void DetachPreviewInterest()
    {
        if (_previewOwner is null) return;
        _previewOwner.PropertyChanged -= OnPreviewOwnerChanged;
        _previewOwner.SetInputPreviewVisible(false);
        _previewOwner = null;
    }

    private void OnPreviewOwnerChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs args)
    {
        if (args.PropertyName is nameof(StudioViewModel.IsInputsTab) or nameof(StudioViewModel.ActiveTab))
            if (_previewOwner is { } owner) owner.SetInputPreviewVisible(owner.IsInputsTab);
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
            typeof(SourcesInputsPage),
            new PropertyMetadata(null, (sender, _) =>
            {
                if (sender is SourcesInputsPage { IsLoaded: true } page) page.AttachPreviewInterest();
            }));

    private void OnSourceCatalogRequested(object? sender, EventArgs e)
    {
        var network = SourcesWorkspace.State.Category == "Network";
        CaptureCatalog.IsExpanded = !network;
        SrtCatalog.IsExpanded = network;
        RtmpCatalog.IsExpanded = network;
        (network ? SrtCatalog : CaptureCatalog).StartBringIntoView();
    }
}
