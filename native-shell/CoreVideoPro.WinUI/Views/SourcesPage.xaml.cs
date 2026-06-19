using System.Collections.Specialized;
using System.ComponentModel;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class SourcesPage : UserControl
{
    private readonly DispatcherQueue? _dispatcher = DispatcherQueue.GetForCurrentThread();
    private StudioViewModel? _boundViewModel;
    private bool _previewSlotListRefreshScheduled;

    public SourcesPage()
    {
        InitializeComponent();
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
        SchedulePreviewSlotListRefresh();

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
            previous.PreviewSlotEditors.CollectionChanged -= OnPreviewSlotEditorsChanged;
        }

        _boundViewModel = next;

        if (next is not null)
        {
            next.PropertyChanged += OnViewModelPropertyChanged;
            next.PreviewSlotEditors.CollectionChanged += OnPreviewSlotEditorsChanged;
        }

        SchedulePreviewSlotListRefresh();
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.PreviewSlotEditors)
            or nameof(StudioViewModel.HasPreviewSlotEditors))
        {
            SchedulePreviewSlotListRefresh();
        }
    }

    private void OnPreviewSlotEditorsChanged(object? sender, NotifyCollectionChangedEventArgs e) =>
        SchedulePreviewSlotListRefresh();

    private void SchedulePreviewSlotListRefresh()
    {
        if (_previewSlotListRefreshScheduled || _dispatcher is null)
        {
            return;
        }

        _previewSlotListRefreshScheduled = true;
        _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _previewSlotListRefreshScheduled = false;
            PreviewSlotListView.ItemsSource = null;
            _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
            {
                PreviewSlotListView.ItemsSource = ViewModel?.PreviewSlotEditors;
            });
        });
    }

    private void OnShowInputEditorPrepared(ItemsRepeater sender, ItemsRepeaterElementPreparedEventArgs args)
    {
        if (args.Element is not FrameworkElement root)
        {
            return;
        }

        var kindCombo = FindDescendant<ComboBox>(root, "KindCombo");
        if (kindCombo is null)
        {
            return;
        }

        kindCombo.SelectionChanged -= OnShowInputKindChanged;
        kindCombo.SelectionChanged += OnShowInputKindChanged;
    }

    private void OnShowInputKindChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox combo ||
            combo.DataContext is not ShowInputSlotViewModel editor ||
            combo.SelectedValue is not ShowInputKind kind)
        {
            return;
        }

        editor.Kind = kind;
    }

    private static T? FindDescendant<T>(DependencyObject root, string name) where T : FrameworkElement
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var index = 0; index < count; index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T match && match.Name == name)
            {
                return match;
            }

            var descendant = FindDescendant<T>(child, name);
            if (descendant is not null)
            {
                return descendant;
            }
        }

        return null;
    }
}