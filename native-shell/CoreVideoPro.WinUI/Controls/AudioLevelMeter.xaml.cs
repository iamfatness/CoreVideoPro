using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace CoreVideoPro.WinUI.Controls;

public sealed partial class AudioLevelMeter : UserControl
{
    public static readonly DependencyProperty LevelProperty =
        DependencyProperty.Register(
            nameof(Level),
            typeof(double),
            typeof(AudioLevelMeter),
            new PropertyMetadata(0.0, OnLevelPropertyChanged));

    public static readonly DependencyProperty IsVerticalProperty =
        DependencyProperty.Register(
            nameof(IsVertical),
            typeof(bool),
            typeof(AudioLevelMeter),
            new PropertyMetadata(false, OnMeterPropertyChanged));

    public static readonly DependencyProperty SegmentCountProperty =
        DependencyProperty.Register(
            nameof(SegmentCount),
            typeof(int),
            typeof(AudioLevelMeter),
            new PropertyMetadata(18, OnMeterPropertyChanged));

    public static readonly DependencyProperty IsMutedProperty =
        DependencyProperty.Register(
            nameof(IsMuted),
            typeof(bool),
            typeof(AudioLevelMeter),
            new PropertyMetadata(false, OnMutedPropertyChanged));

    public static readonly DependencyProperty ShowDbfsScaleProperty =
        DependencyProperty.Register(
            nameof(ShowDbfsScale),
            typeof(bool),
            typeof(AudioLevelMeter),
            new PropertyMetadata(false, OnMeterPropertyChanged));

    // #481: while muted, a channel meter can still be told to show the PRE-MUTE
    // input level (so the A1 sees a muted guest talking) instead of the usual
    // hard-zero. When this is set, IsMuted no longer zeroes the bar; it only
    // switches the fill to a visually distinct dim color so the strip still
    // reads as muted at a glance.
    public static readonly DependencyProperty ShowLevelWhileMutedProperty =
        DependencyProperty.Register(
            nameof(ShowLevelWhileMuted),
            typeof(bool),
            typeof(AudioLevelMeter),
            new PropertyMetadata(false, OnMutedPropertyChanged));

    private readonly SolidColorBrush DimBrush = new(Windows.UI.Color.FromArgb(255, 21, 30, 34));
    private readonly SolidColorBrush GreenBrush = new(Windows.UI.Color.FromArgb(255, 46, 210, 116));
    private readonly SolidColorBrush YellowBrush = new(Windows.UI.Color.FromArgb(255, 245, 190, 69));
    private readonly SolidColorBrush RedBrush = new(Windows.UI.Color.FromArgb(255, 237, 76, 68));
    // Dimmed blue-gray: distinguishes "muted, but this is the input level" from
    // a normal live green/yellow/red bar at the same height.
    private readonly SolidColorBrush MutedInputBrush = new(Windows.UI.Color.FromArgb(255, 92, 122, 145));

    private readonly Models.AudioMeterBallistics _ballistics = new();
    private readonly Border[] _segments = new Border[48];
    private readonly StackPanel _segmentPanel = new();
    private readonly Canvas _scale = new();
    private readonly double[] _tickValues = [0, -6, -12, -24, -30, -36, -48, -60];
    private readonly Border[] _ticks = new Border[8];
    private readonly TextBlock[] _labels = new TextBlock[8];
    private readonly ColumnDefinition _barColumn = new();
    private DispatcherQueueTimer? _decayTimer;
    private (bool Vertical, bool Scale, int Count, double Size, double Spacing, double Available)? _layout;

    public AudioLevelMeter()
    {
        InitializeComponent();
        // A bounded, retained visual tree. No level, resize, orientation or
        // scale update removes controls for the finalizer to race releasing.
        RootGrid.ColumnDefinitions.Add(_barColumn);
        RootGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        RootGrid.Children.Add(_segmentPanel);
        Grid.SetColumn(_scale, 1);
        RootGrid.Children.Add(_scale);
        for (var i = 0; i < _segments.Length; i++)
        {
            var segment = new Border { CornerRadius = new CornerRadius(1.5), Visibility = Visibility.Collapsed };
            _segments[i] = segment;
            _segmentPanel.Children.Add(segment);
        }
        var tickBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(180, 126, 145, 156));
        var labelBrush = new SolidColorBrush(Windows.UI.Color.FromArgb(220, 160, 177, 187));
        for (var i = 0; i < _tickValues.Length; i++)
        {
            _ticks[i] = new Border { Width = 4, Height = 1, Background = tickBrush };
            _labels[i] = new TextBlock { Text = _tickValues[i].ToString("0"), FontSize = 7, Foreground = labelBrush };
            Canvas.SetLeft(_labels[i], 6);
            _scale.Children.Add(_ticks[i]);
            _scale.Children.Add(_labels[i]);
        }
        Loaded += OnLoaded;
        SizeChanged += (_, _) => { if (IsLoaded) RenderSegments(); };
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs args)
    {
        _ballistics.Reset(Level, IsMuted, ShowLevelWhileMuted, Environment.TickCount64);
        RenderSegments();
    }

    private void OnUnloaded(object sender, RoutedEventArgs args)
    {
        if (_decayTimer is not null)
        {
            _decayTimer.Stop();
            _decayTimer.Tick -= OnDecayTick;
            _decayTimer = null;
        }
    }

    // Exposed only inside this assembly for the isolated real-XAML stress probe.
    internal bool AnimationRunning => _decayTimer?.IsRunning == true;

    public double Level
    {
        get => (double)GetValue(LevelProperty);
        set => SetValue(LevelProperty, value);
    }

    public bool IsVertical
    {
        get => (bool)GetValue(IsVerticalProperty);
        set => SetValue(IsVerticalProperty, value);
    }

    public int SegmentCount
    {
        get => (int)GetValue(SegmentCountProperty);
        set => SetValue(SegmentCountProperty, value);
    }

    public bool IsMuted
    {
        get => (bool)GetValue(IsMutedProperty);
        set => SetValue(IsMutedProperty, value);
    }

    public bool ShowDbfsScale
    {
        get => (bool)GetValue(ShowDbfsScaleProperty);
        set => SetValue(ShowDbfsScaleProperty, value);
    }

    public bool ShowLevelWhileMuted
    {
        get => (bool)GetValue(ShowLevelWhileMutedProperty);
        set => SetValue(ShowLevelWhileMutedProperty, value);
    }

    private static void OnLevelPropertyChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs args)
    {
        if (dependencyObject is AudioLevelMeter meter)
        {
            meter.OnLevelChanged();
        }
    }

    private static void OnMeterPropertyChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs args)
    {
        if (dependencyObject is AudioLevelMeter meter && meter.IsLoaded)
        {
            meter.RenderSegments();
        }
    }

    private static void OnMutedPropertyChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs args)
    {
        if (dependencyObject is AudioLevelMeter meter) meter.OnLevelChanged();
    }

    private void OnLevelChanged()
    {
        // Binding updates can still arrive for an unloaded page. Loaded will
        // reconcile the newest Level; hidden meters must not restart timers.
        if (!IsLoaded) return;
        _ballistics.SetInput(Level, IsMuted, ShowLevelWhileMuted, Environment.TickCount64);
        RenderSegments();
        UpdateTimer();
    }

    private void UpdateTimer()
    {
        if (!IsLoaded || !_ballistics.NeedsAnimation)
        {
            _decayTimer?.Stop();
            return;
        }
        if (_decayTimer is null)
        {
            _decayTimer = DispatcherQueue.CreateTimer();
            _decayTimer.Interval = TimeSpan.FromMilliseconds(33);
            _decayTimer.IsRepeating = true;
            _decayTimer.Tick += OnDecayTick;
        }
        if (!_decayTimer.IsRunning) _decayTimer.Start();
    }

    private void OnDecayTick(DispatcherQueueTimer sender, object args)
    {
        if (!IsLoaded) { sender.Stop(); return; }
        _ballistics.Advance(Environment.TickCount64);
        RenderSegments();
        UpdateTimer();
    }

    private void RenderSegments()
    {
        var availableMain = IsVertical ? RootGrid.ActualHeight : RootGrid.ActualWidth;
        var fit = IsVertical
            ? Models.AudioMeterScale.FitVerticalSegments(availableMain, SegmentCount)
            : Models.AudioMeterScale.FitHorizontalSegments(availableMain, SegmentCount);
        var count = fit.SegmentCount;
        var segMain = fit.SegmentSize;
        var spacing = fit.Spacing;
        var activeSegments = (int)Math.Round(_ballistics.Level / 100.0 * count, MidpointRounding.AwayFromZero);
        var peakSegment = _ballistics.Peak > 0
            ? (int)Math.Round(_ballistics.Peak / 100.0 * count, MidpointRounding.AwayFromZero) - 1
            : -1;

        var scaleVisible = IsVertical && ShowDbfsScale && availableMain > 0;
        var layoutKey = (IsVertical, scaleVisible, count, segMain, spacing, availableMain);
        if (_layout != layoutKey)
        {
            _layout = layoutKey;
            _segmentPanel.Orientation = IsVertical ? Orientation.Vertical : Orientation.Horizontal;
            _segmentPanel.HorizontalAlignment = IsVertical ? HorizontalAlignment.Center : HorizontalAlignment.Stretch;
            _segmentPanel.VerticalAlignment = IsVertical ? VerticalAlignment.Bottom : VerticalAlignment.Center;
            _segmentPanel.Spacing = spacing;
            _barColumn.Width = scaleVisible ? new GridLength(14) : new GridLength(1, GridUnitType.Star);
            Grid.SetColumnSpan(_segmentPanel, scaleVisible ? 1 : 2);
            _scale.Visibility = scaleVisible ? Visibility.Visible : Visibility.Collapsed;
            for (var i = 0; i < _segments.Length; i++)
            {
                _segments[i].Visibility = i < count ? Visibility.Visible : Visibility.Collapsed;
                _segments[i].Width = IsVertical ? 14 : segMain;
                _segments[i].Height = IsVertical ? segMain : 10;
            }
            if (scaleVisible) UpdateScale(availableMain);
        }
        for (var visualIndex = 0; visualIndex < count; visualIndex++)
        {
            var lowToHighIndex = IsVertical ? count - visualIndex - 1 : visualIndex;
            var lit = lowToHighIndex < activeSegments || lowToHighIndex == peakSegment;
            var brush = lit
                ? (IsMuted && ShowLevelWhileMuted ? MutedInputBrush : BrushFor((lowToHighIndex + 1) / (double)count))
                : DimBrush;
            if (!ReferenceEquals(_segments[visualIndex].Background, brush))
                _segments[visualIndex].Background = brush;
        }
    }

    private void UpdateScale(double height)
    {
        _scale.Height = height;
        for (var i = 0; i < _tickValues.Length; i++)
        {
            var dbfs = _tickValues[i];
            var visible = height < 60 ? dbfs is 0 or -30 or -60
                : height < 100 ? dbfs != -6 && dbfs != -30 : dbfs != -30;
            _ticks[i].Visibility = _labels[i].Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            var y = (1 - Models.AudioMeterScale.ToLevel(dbfs) / 100.0) * height;
            Canvas.SetTop(_ticks[i], Math.Clamp(y, 0, Math.Max(0, height - 1)));
            Canvas.SetTop(_labels[i], Math.Clamp(y - 6, 0, Math.Max(0, height - 12)));
        }
    }

    private SolidColorBrush BrushFor(double normalized) =>
        normalized >= 0.9 ? RedBrush : normalized >= 0.7 ? YellowBrush : GreenBrush;
}
