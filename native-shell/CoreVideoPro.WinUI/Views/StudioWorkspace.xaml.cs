using System.ComponentModel;
using CoreVideoPro.WinUI.Services;
using CoreVideoPro.WinUI.ViewModels;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace CoreVideoPro.WinUI.Views;

public sealed partial class StudioWorkspace : UserControl
{
    private readonly DispatcherQueue? _dispatcher = DispatcherQueue.GetForCurrentThread();
    private StudioViewModel? _boundViewModel;
    private bool _participantListRefreshScheduled;

    public static readonly DependencyProperty ViewModelProperty =
        DependencyProperty.Register(
            nameof(ViewModel),
            typeof(StudioViewModel),
            typeof(StudioWorkspace),
            new PropertyMetadata(null, OnViewModelChanged));

    public StudioWorkspace()
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

    private static void OnViewModelChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args) =>
        ((StudioWorkspace)sender).BindViewModel(
            (StudioViewModel?)args.NewValue,
            (StudioViewModel?)args.OldValue);

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        ScheduleParticipantListRefresh();
    }

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
        }

        _boundViewModel = next;

        if (next is not null)
        {
            next.PropertyChanged += OnViewModelPropertyChanged;
        }

        ScheduleParticipantListRefresh();
    }

    private void OnViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.ParticipantListItems))
        {
            ScheduleParticipantListRefresh();
        }
    }

    private void ScheduleParticipantListRefresh()
    {
        if (_participantListRefreshScheduled || _dispatcher is null)
        {
            return;
        }

        _participantListRefreshScheduled = true;
        _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            _participantListRefreshScheduled = false;
            ParticipantListView.ItemsSource = null;
            _dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
            {
                ParticipantListView.ItemsSource = ViewModel?.ParticipantListItems;
            });
        });
    }

    // Owner: the room panel is redundant with the Sources tab. The X on the
    // panel hides it; the "Room" toggle in the multiviewer header brings it back.
    private void OnHideRoomPanelClicked(object sender, RoutedEventArgs args)
    {
        if (ViewModel is not null)
        {
            ViewModel.ShowRoomPanel = false;
        }
    }
}
