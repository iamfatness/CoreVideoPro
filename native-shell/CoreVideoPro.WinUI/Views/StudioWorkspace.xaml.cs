using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using CoreVideoPro.MediaCore.Services;
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
    private DispatcherTimer? _headerClockTimer;

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

    /// <summary>The nav bar, exposed so the window can use it as the title-bar
    /// drag region (the app draws its own top bar; no separate caption strip).</summary>
    public FrameworkElement TitleBarDragRegion => NavTitleBar;

    private static void OnViewModelChanged(DependencyObject sender, DependencyPropertyChangedEventArgs args) =>
        ((StudioWorkspace)sender).BindViewModel(
            (StudioViewModel?)args.NewValue,
            (StudioViewModel?)args.OldValue);

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        ScheduleParticipantListRefresh();
        StartHeaderClock();
        BeginCrashReportScan();
    }

    // ==== S1 crash pipeline (docs/beta-engineering-spec.md §S1) ====
    // Launch-time dump detection is async and OFF the startup critical path; the
    // InfoBar is a static one-shot surface driven entirely from this code-behind
    // (no bound collections — 0xc000027b rules). Nothing is ever auto-sent.

    private static int _crashScanStarted; // once per process, not per load
    private CrashReportCoordinator? _crashCoordinator;
    private IReadOnlyList<CrashDumpFile>? _pendingCrashDumps;

    private void BeginCrashReportScan()
    {
        if (Interlocked.Exchange(ref _crashScanStarted, 1) == 1)
        {
            return;
        }

        var coordinator = CrashReportCoordinator.CreateFromEnvironment();
        if (coordinator is null)
        {
            LaunchLog.Write("crash-report: telemetry endpoint/key not configured — crash reporting disabled");
            return;
        }

        _crashCoordinator = coordinator;
        _ = RunCrashReportScanAsync(coordinator);
    }

    private async Task RunCrashReportScanAsync(CrashReportCoordinator coordinator)
    {
        try
        {
            var dumps = await Task.Run(coordinator.ScanAndAdvanceWatermark).ConfigureAwait(true);
            if (dumps.Count == 0)
            {
                return;
            }

            _pendingCrashDumps = dumps;
            var newest = dumps[^1];
            CrashReportBar.Message =
                $"A crash dump from {newest.ProcessName} was found " +
                $"({(dumps.Count == 1 ? "1 dump" : $"{dumps.Count} dumps")}). " +
                "Send a report to help fix it? The report (dump + recent logs, secrets redacted) " +
                "is saved locally either way — nothing is sent unless you click Send.";
            CrashReportBar.IsOpen = true;
        }
        catch (Exception ex)
        {
            // The crash reporter must never take the studio down.
            LaunchLog.Write($"crash-report: launch scan failed {ex.GetType().Name}: {ex.Message}");
        }
    }

    private async void OnCrashReportSendClicked(object sender, RoutedEventArgs e)
    {
        var coordinator = _crashCoordinator;
        var dumps = _pendingCrashDumps;
        if (coordinator is null || dumps is null || dumps.Count == 0)
        {
            CrashReportBar.IsOpen = false;
            return;
        }

        CrashReportSendButton.IsEnabled = false;
        CrashReportBar.Message = "Assembling and sending the crash report…";

        try
        {
            var settings = (_boundViewModel ?? ViewModel)?.Settings;
            Func<string, Task<bool>> writeBundle = settings is null
                ? static _ => Task.FromResult(false)
                : settings.TryWriteSupportBundleSnapshotAsync;

            var outcome = await coordinator.SendAsync(dumps, writeBundle).ConfigureAwait(true);

            CrashReportBar.Severity = outcome.Success ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
            CrashReportBar.Title = outcome.Success ? "Crash report sent" : "Crash report not sent";
            CrashReportBar.Message = outcome.Message;
            if (outcome.Success || !outcome.CanRetry)
            {
                _pendingCrashDumps = null;
                CrashReportSendButton.Visibility = Visibility.Collapsed;
            }
            else
            {
                CrashReportSendButton.Content = "Try again";
                CrashReportSendButton.IsEnabled = true;
            }
        }
        catch (Exception ex)
        {
            LaunchLog.Write($"crash-report: send failed {ex.GetType().Name}: {ex.Message}");
            CrashReportBar.Severity = InfoBarSeverity.Error;
            CrashReportBar.Title = "Crash report not sent";
            CrashReportBar.Message = $"Sending failed: {ex.Message}";
            CrashReportSendButton.Content = "Try again";
            CrashReportSendButton.IsEnabled = true;
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _headerClockTimer?.Stop();
        SetPreviewLayoutOverlayOpen(false);
        BindViewModel(null, _boundViewModel);
    }

    // ==== POS-1: preview-layout editing overlay (sources-redesign-spec §B2) ====
    // Same wiring contract as the Scenes tab (SourcesPage): feed PreviewCanvasLayers,
    // commit via CommitPreviewCanvasLayer (the S2b preview DRAFT — program untouched
    // until Take/Update), gate snapshot refreshes during interaction.

    private bool _previewLayoutEditorWired;

    private void OnEditPreviewLayoutToggled(object sender, RoutedEventArgs e) =>
        SetPreviewLayoutOverlayOpen(EditLayoutToggle.IsChecked == true);

    // POS-2: the Add-overlay menu is rebuilt each time it opens so it always reflects
    // the current media bin / role set (a one-shot build would go stale).
    private void OnAddOverlayFlyoutOpening(object sender, object e) =>
        OverlayLayerMenuBuilder.Populate(AddOverlayFlyout, _boundViewModel ?? ViewModel);

    private void OnEditPreviewLayoutDoneClicked(object sender, RoutedEventArgs e)
    {
        EditLayoutToggle.IsChecked = false;
        SetPreviewLayoutOverlayOpen(false);
    }

    private void SetPreviewLayoutOverlayOpen(bool open)
    {
        if (PreviewLayoutOverlay is null)
        {
            return;
        }

        PreviewLayoutOverlay.Visibility = open ? Visibility.Visible : Visibility.Collapsed;
        if (open)
        {
            if (!_previewLayoutEditorWired)
            {
                StudioSceneCanvasEditor.LayerChanged += OnStudioCanvasLayerChanged;
                StudioSceneCanvasEditor.PresetRequested += OnStudioCanvasPresetRequested;
                StudioSceneCanvasEditor.InteractionChanged += OnStudioCanvasInteractionChanged;
                _previewLayoutEditorWired = true;
            }

            if (_boundViewModel is not null)
            {
                _boundViewModel.PropertyChanged += OnPreviewLayoutVmPropertyChanged;
                _boundViewModel.PreviewCanvasLayers.CollectionChanged += OnPreviewLayoutLayersChanged;
            }

            RefreshStudioSceneCanvasEditor();
        }
        else
        {
            if (_boundViewModel is not null)
            {
                _boundViewModel.PropertyChanged -= OnPreviewLayoutVmPropertyChanged;
                _boundViewModel.PreviewCanvasLayers.CollectionChanged -= OnPreviewLayoutLayersChanged;
                _boundViewModel.SetCanvasInteractionActive(false);
            }
        }
    }

    private void OnPreviewLayoutVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(StudioViewModel.PreviewCanvasLayers))
        {
            RefreshStudioSceneCanvasEditor();
        }
    }

    private void OnPreviewLayoutLayersChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (StudioSceneCanvasEditor.IsInteracting)
        {
            return;
        }

        RefreshStudioSceneCanvasEditor();
    }

    private void RefreshStudioSceneCanvasEditor()
    {
        var layers = _boundViewModel?.PreviewCanvasLayers.ToList();
        StudioSceneCanvasEditor.SetLayers(layers, layers?.FirstOrDefault());
    }

    private void OnStudioCanvasLayerChanged(object? sender, SceneCanvasLayerViewModel layer) =>
        _boundViewModel?.CommitPreviewCanvasLayer(layer);

    private void OnStudioCanvasPresetRequested(object? sender, string preset) =>
        _boundViewModel?.ApplyCanvasPreset(preset);

    private void OnStudioCanvasInteractionChanged(object? sender, bool isInteracting)
    {
        _boundViewModel?.SetCanvasInteractionActive(isInteracting);
        if (!isInteracting)
        {
            RefreshStudioSceneCanvasEditor();
        }
    }

    // Drives the wall-clock shown next to the P/P out button (HH:mm:ss, ticked once a
    // second) -- the clock the operator asked to move out of the program-tile corner.
    private void StartHeaderClock()
    {
        _headerClockTimer ??= new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _headerClockTimer.Tick -= OnHeaderClockTick;
        _headerClockTimer.Tick += OnHeaderClockTick;
        OnHeaderClockTick(this, null);
        _headerClockTimer.Start();
    }

    private void OnHeaderClockTick(object? sender, object? e) =>
        HeaderClockText.Text = CoreVideoPro.WinUI.Controls.MultiviewOverlayFormatting.FormatClock(DateTime.Now);

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
