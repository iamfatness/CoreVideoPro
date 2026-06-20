using System.ComponentModel;
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
    private bool _programPreviewSplitterDragging;

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

    private void OnLoaded(object sender, RoutedEventArgs e) =>
        ScheduleParticipantListRefresh();

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

    private void OnProgramPreviewSplitterPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        _programPreviewSplitterDragging = true;
        ProgramPreviewSplitter.CapturePointer(e.Pointer);
        e.Handled = true;
    }

    private void OnProgramPreviewSplitterPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_programPreviewSplitterDragging)
        {
            return;
        }

        var pointerX = e.GetCurrentPoint(ProgramPreviewGrid).Position.X;
        ResizeProgramPreviewColumns(pointerX);
        e.Handled = true;
    }

    private void OnProgramPreviewSplitterPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (!_programPreviewSplitterDragging)
        {
            return;
        }

        _programPreviewSplitterDragging = false;
        ProgramPreviewSplitter.ReleasePointerCapture(e.Pointer);
        e.Handled = true;
    }

    private void ResizeProgramPreviewColumns(double pointerX)
    {
        const double minMonitorWidth = 220;
        const double columnSpacingTotal = 8;
        var availableWidth = ProgramPreviewGrid.ActualWidth - ProgramPreviewSplitter.ActualWidth - columnSpacingTotal;
        if (availableWidth <= minMonitorWidth * 2)
        {
            return;
        }

        var previewWidth = Math.Clamp(pointerX, minMonitorWidth, availableWidth - minMonitorWidth);
        var programWidth = availableWidth - previewWidth;

        PreviewMonitorColumn.Width = new GridLength(previewWidth, GridUnitType.Star);
        ProgramMonitorColumn.Width = new GridLength(programWidth, GridUnitType.Star);
    }
}
