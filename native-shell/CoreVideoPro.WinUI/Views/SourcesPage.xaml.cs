using System.Collections.Specialized;
using System.ComponentModel;
using CoreVideoPro.WinUI.ViewModels;
using CoreVideoPro.WinUI.Models;
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

    private void OnLayerSourceComboLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is ComboBox combo)
            RestoreLayerSourceCombo(combo);
    }

    private void OnLayerSourceDropDownOpened(object sender, object e)
    {
        if (sender is ComboBox { Tag: SceneCanvasLayerViewModel layer })
            layer.BeginOperatorSourcePick();
    }

    private void OnLayerSourceDropDownClosed(object sender, object e)
    {
        if (sender is not ComboBox { Tag: SceneCanvasLayerViewModel layer } combo)
            return;
        // Selection has settled; SelectedItem is the pick. End the gesture
        // now so a later ItemsSource rebuild cannot ride the operator flag.
        CommitLayerSourceCombo(combo, operatorGesture: true, added: null);
        layer.EndOperatorSourcePick();
    }

    private void OnLayerSourceSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        // Tag (x:Bind), not DataContext — null inside the ItemsRepeater.
        // A list rebuild adds the blank placeholder; that is not an operator pick.
        if (sender is not ComboBox { Tag: SceneCanvasLayerViewModel layer } combo)
            return;
        var added = e.AddedItems.Count == 1 ? e.AddedItems[0] as RouteSelectOption : null;
        if (layer.IsOperatorPickingSource)
        {
            CommitLayerSourceCombo(combo, operatorGesture: true, added);
            return;
        }

        LaunchLog.Write(LayerSourceSelectionPolicy.FormatLog(
            layer.LayerIndex, added?.Value ?? combo.SelectedValue as string,
            LayerSourceSelectionPolicy.RefreshIgnoredCause));
        RestoreLayerSourceCombo(combo);
    }

    private void CommitLayerSourceCombo(ComboBox combo, bool operatorGesture, RouteSelectOption? added)
    {
        if (combo.Tag is not SceneCanvasLayerViewModel layer)
            return;
        var option = added;
        if (option is null && combo.SelectedItem is RouteSelectOption selected)
            option = selected;
        if (option is null && combo.SelectedValue is string value)
            option = layer.ParticipantOptions.FirstOrDefault(item => item.Value == value);
        if (layer.TryCommitSourceOption(option, operatorGesture))
        {
            LaunchLog.Write(LayerSourceSelectionPolicy.FormatLog(
                layer.LayerIndex, option?.Value, LayerSourceSelectionPolicy.OperatorCause));
        }
    }

    private void RestoreLayerSourceCombo(ComboBox combo)
    {
        if (combo.Tag is not SceneCanvasLayerViewModel layer)
            return;
        try
        {
            combo.SelectionChanged -= OnLayerSourceSelectionChanged;
            var value = layer.ParticipantId ?? string.Empty;
            if (layer.ParticipantOptions.Any(option => option.Value == value))
                combo.SelectedValue = value;
            combo.SelectionChanged += OnLayerSourceSelectionChanged;
        }
        catch (Exception ex)
        {
            combo.SelectionChanged -= OnLayerSourceSelectionChanged;
            combo.SelectionChanged += OnLayerSourceSelectionChanged;
            LaunchLog.Write($"scene source selection skipped ({ex.GetType().Name}: {ex.Message})");
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
