using System.Collections.Specialized;
using System.ComponentModel;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesPage : UserControl
{
    private readonly LoadedViewModelSubscription<StudioViewModel> _subscriptions;
    private bool _scenePickerRestoreScheduled;

    public SourcesPage()
    {
        _subscriptions = new(SubscribeViewModel, UnsubscribeViewModel);
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

    private static void OnViewModelChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args)
    {
        var page = (SourcesPage)sender;
        var viewModel = (StudioViewModel?)args.NewValue;
        page._subscriptions.SetViewModel(viewModel);
        page.PopulateAddSourceFlyout(viewModel);
        page.RefreshSceneCanvasEditor();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Pages may be removed and reinserted without their ViewModel DP changing.
        // Restore listeners before reading the current scene on every load.
        _subscriptions.Load(ViewModel);
        PopulateAddSourceFlyout(ViewModel);
        RefreshSceneCanvasEditor();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _subscriptions.Unload();
        SceneCanvasEditor.SetCompositeSurface(null);
    }

    private void SubscribeViewModel(StudioViewModel viewModel)
    {
        viewModel.PropertyChanged += OnViewModelPropertyChanged;
        viewModel.PreviewCanvasLayers.CollectionChanged += OnPreviewCanvasLayersChanged;
    }

    private void UnsubscribeViewModel(StudioViewModel viewModel)
    {
        viewModel.PropertyChanged -= OnViewModelPropertyChanged;
        viewModel.PreviewCanvasLayers.CollectionChanged -= OnPreviewCanvasLayersChanged;
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

    // POS-2: rebuilt on every open so it always reflects the current media bin
    // (the one-shot Add-source population above would show a stale asset list).
    private void OnAddOverlayFlyoutOpening(object sender, object e) =>
        OverlayLayerMenuBuilder.Populate(AddOverlayFlyout, _subscriptions.Current ?? ViewModel);

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(StudioViewModel.SceneItems) && !_scenePickerRestoreScheduled)
        {
            _scenePickerRestoreScheduled = true;
            UiDispatch.Enqueue(DispatcherQueue, Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, () =>
            {
                _scenePickerRestoreScheduled = false;
                if (IsLoaded) ScenePicker.SelectedValue = ViewModel?.PreviewSceneId;
            }, "scene-picker.restore-selection");
        }
        if (e.PropertyName is nameof(StudioViewModel.SceneCanvasCompositeSurface))
        {
            SceneCanvasEditor.SetCompositeSurface(ViewModel?.SceneCanvasCompositeSurface);
            return;
        }
        if (e.PropertyName is nameof(StudioViewModel.PreviewCanvasLayers)
            or nameof(StudioViewModel.HasPreviewSlotEditors)
            or nameof(StudioViewModel.PreviewSceneBackgroundAsset)
            or nameof(StudioViewModel.PreviewSceneId))
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
        SceneCanvasEditor.SetCompositeSurface(ViewModel?.SceneCanvasCompositeSurface);
        var layers = ViewModel?.PreviewCanvasLayers.ToList();
        SceneCanvasEditor.SetBackground(ViewModel?.PreviewSceneBackgroundAsset);
        SceneCanvasEditor.SetLayers(layers, layers?.FirstOrDefault());
    }

    private void OnCanvasPresetRequested(object? sender, string preset) =>
        ViewModel?.ApplyCanvasPreset(preset);

    private void OnLayerSourceSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is ComboBox { SelectedValue: string value, DataContext: SceneCanvasLayerViewModel layer })
        {
            layer.TrySelectSource(value);
        }
    }

    private void OnPreviewSceneSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        // ItemsSource refresh clears selection temporarily. Only a real scene
        // selection may cue preview; a binding reset must never become a Take target.
        if (sender is ComboBox { SelectedValue: string sceneId } &&
            ViewModel is { } viewModel &&
            viewModel.Scenes.Any(scene => string.Equals(scene.Id, sceneId, StringComparison.Ordinal)) &&
            !string.Equals(viewModel.PreviewSceneId, sceneId, StringComparison.Ordinal))
        {
            viewModel.SelectSceneCommand.Execute(sceneId);
        }
    }

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
