using System.Collections.Specialized;
using System.ComponentModel;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesPage : UserControl
{
    private StudioViewModel? _boundViewModel;

    public SourcesPage()
    {
        InitializeComponent();
        SceneCanvasEditor.PresetRequested += OnCanvasPresetRequested;
        SceneCanvasEditor.LayerChanged += OnCanvasLayerChanged;
        SceneCanvasEditor.InteractionChanged += OnCanvasInteractionChanged;
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
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
            typeof(SourcesPage),
            new PropertyMetadata(null, OnViewModelChanged));

    private static void OnViewModelChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args) =>
        ((SourcesPage)sender).BindViewModel(
            (StudioViewModel?)args.NewValue,
            (StudioViewModel?)args.OldValue);

    private void OnLoaded(object sender, RoutedEventArgs e) =>
        RefreshSceneCanvasEditor();

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        BindViewModel(null, _boundViewModel);

    private void BindViewModel(StudioViewModel? next, StudioViewModel? previous)
    {
        if (ReferenceEquals(previous, next))
        {
            return;
        }

        if (previous is not null)
        {
            previous.PropertyChanged -= OnViewModelPropertyChanged;
            previous.PreviewCanvasLayers.CollectionChanged -= OnPreviewCanvasLayersChanged;
        }

        _boundViewModel = next;

        if (next is not null)
        {
            next.PropertyChanged += OnViewModelPropertyChanged;
            next.PreviewCanvasLayers.CollectionChanged += OnPreviewCanvasLayersChanged;
        }

        PopulateAddSourceFlyout(next);
        RefreshSceneCanvasEditor();
    }

    private void PopulateAddSourceFlyout(StudioViewModel? viewModel)
    {
        AddSourceFlyout.Items.Clear();
        if (viewModel is null)
        {
            return;
        }

        foreach (var option in viewModel.AddSourceOptions)
        {
            AddSourceFlyout.Items.Add(new MenuFlyoutItem
            {
                Text = option.Label,
                Command = viewModel.AddCanvasSourceCommand,
                CommandParameter = option.Value
            });
        }
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.PreviewCanvasLayers)
            or nameof(StudioViewModel.HasPreviewSlotEditors))
        {
            RefreshSceneCanvasEditor();
        }
    }

    private void OnPreviewCanvasLayersChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (SceneCanvasEditor.IsInteracting)
        {
            return;
        }

        if (e.Action is NotifyCollectionChangedAction.Reset or NotifyCollectionChangedAction.Add
            or NotifyCollectionChangedAction.Remove or NotifyCollectionChangedAction.Replace)
        {
            RefreshSceneCanvasEditor();
        }
    }

    private void RefreshSceneCanvasEditor()
    {
        var layers = ViewModel?.PreviewCanvasLayers.ToList();
        SceneCanvasEditor.SetLayers(layers, layers?.FirstOrDefault());
    }

    private void OnCanvasPresetRequested(object? sender, string preset) =>
        ViewModel?.ApplyCanvasPreset(preset);

    private void OnCanvasLayerChanged(object? sender, SceneCanvasLayerViewModel layer) =>
        ViewModel?.CommitPreviewCanvasLayer(layer);

    private void OnCanvasInteractionChanged(object? sender, bool isInteracting)
    {
        ViewModel?.SetCanvasInteractionActive(isInteracting);
        if (!isInteracting)
        {
            RefreshSceneCanvasEditor();
        }
    }

    // S1 layer primitives: the card buttons live inside an ItemsRepeater
    // DataTemplate, so they reach the page ViewModel's commands through the
    // element's DataContext (the layer VM) here.
    private void OnRemoveLayerClicked(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is SceneCanvasLayerViewModel layer)
        {
            ViewModel?.RemoveCanvasSourceCommand.Execute(layer);
        }
    }

    // S3b: tapping a layer card selects its box on the canvas (the reverse
    // direction — canvas press highlighting the card — flows through
    // SceneCanvasLayerViewModel.IsSelected, set by the editor control).
    private void OnLayerCardTapped(object sender, Microsoft.UI.Xaml.Input.TappedRoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is SceneCanvasLayerViewModel layer)
        {
            SceneCanvasEditor.SelectLayer(layer);
        }
    }

    private void OnMoveLayerForwardClicked(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is SceneCanvasLayerViewModel layer)
        {
            ViewModel?.MoveCanvasSourceForwardCommand.Execute(layer);
        }
    }

    private void OnMoveLayerBackClicked(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.DataContext is SceneCanvasLayerViewModel layer)
        {
            ViewModel?.MoveCanvasSourceBackCommand.Execute(layer);
        }
    }
}