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

    private static readonly SolidColorBrush DimBrush = new(Windows.UI.Color.FromArgb(255, 21, 30, 34));
    private static readonly SolidColorBrush GreenBrush = new(Windows.UI.Color.FromArgb(255, 46, 210, 116));
    private static readonly SolidColorBrush YellowBrush = new(Windows.UI.Color.FromArgb(255, 245, 190, 69));
    private static readonly SolidColorBrush RedBrush = new(Windows.UI.Color.FromArgb(255, 237, 76, 68));

    // Console meter ballistics (audio overhaul spec 4.4): the snapshot delivers
    // INSTANTANEOUS per-tick levels, and the audio quanta vary per tick, so a
    // meter bound raw to Level strobes instead of dancing. Attack is instant
    // (a transient must show immediately); release decays exponentially with
    // ~300ms time constant; a peak-hold segment lingers ~800ms then falls.
    private const double ReleaseFactorPerTick = 0.896;  // exp(-33ms / 300ms)
    private const double ReleaseFloorKick = 0.25;       // finishes the decay tail
    private static readonly TimeSpan PeakHold = TimeSpan.FromMilliseconds(800);
    private const double PeakFallPerTick = 4.0;

    private double _displayedLevel;
    private double _peakLevel;
    private DateTimeOffset _peakSetAt = DateTimeOffset.MinValue;
    private DispatcherQueueTimer? _decayTimer;

    public AudioLevelMeter()
    {
        InitializeComponent();
        Loaded += (_, _) => RenderSegments();
        // Re-fit on ANY size change: segments scale to the available column (see
        // RenderSegments). Without this, a window restored from fullscreen kept
        // the fullscreen-sized stack and clipped the green end of every meter.
        SizeChanged += (_, e) =>
        {
            if (e.NewSize.Height > 0 || e.NewSize.Width > 0)
            {
                RenderSegments();
            }
        };
        Unloaded += (_, _) => StopDecayTimer();
    }

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

    private void OnLevelChanged()
    {
        var target = Math.Clamp(Level, 0, 100);
        if (target >= _displayedLevel)
        {
            _displayedLevel = target;  // instant attack
        }

        if (target >= _peakLevel)
        {
            _peakLevel = target;
            _peakSetAt = DateTimeOffset.UtcNow;
        }

        if (IsLoaded)
        {
            RenderSegments();
        }

        EnsureDecayTimer();
    }

    private void EnsureDecayTimer()
    {
        if (_decayTimer is { IsRunning: true })
        {
            return;
        }

        _decayTimer ??= CreateDecayTimer();
        _decayTimer?.Start();
    }

    private DispatcherQueueTimer? CreateDecayTimer()
    {
        var queue = DispatcherQueue;
        if (queue is null)
        {
            return null;
        }

        var timer = queue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(33);
        timer.IsRepeating = true;
        timer.Tick += (_, _) => DecayTick();
        return timer;
    }

    private void StopDecayTimer() => _decayTimer?.Stop();

    private void DecayTick()
    {
        var target = Math.Clamp(Level, 0, 100);
        var changed = false;

        if (_displayedLevel > target)
        {
            _displayedLevel = Math.Max(target, _displayedLevel * ReleaseFactorPerTick - ReleaseFloorKick);
            changed = true;
        }

        if (_peakLevel > _displayedLevel && DateTimeOffset.UtcNow - _peakSetAt > PeakHold)
        {
            _peakLevel = Math.Max(_displayedLevel, _peakLevel - PeakFallPerTick);
            changed = true;
        }

        if (changed)
        {
            if (IsLoaded)
            {
                RenderSegments();
            }

            return;
        }

        // Converged and peak expired: idle the timer (a page can host a dozen
        // meters; they should cost nothing while silent).
        StopDecayTimer();
    }

    private void RenderSegments()
    {
        var count = Math.Clamp(SegmentCount, 8, 48);
        var level = Math.Clamp(_displayedLevel, 0, 100);
        var activeSegments = (int)Math.Round(level / 100.0 * count, MidpointRounding.AwayFromZero);
        var peakSegment = _peakLevel > 0
            ? (int)Math.Round(Math.Clamp(_peakLevel, 0, 100) / 100.0 * count, MidpointRounding.AwayFromZero) - 1
            : -1;

        // SCALE TO THE SPACE WE HAVE. Fixed 7px segments + 2px spacing need a
        // 324px column at 36 segments; any smaller window CLIPPED the stack —
        // and because the low/green segments sit at the visual bottom, exactly
        // the audible part of the meter vanished ("meters don't work when not
        // in full screen", 2026-08-09). Shrink spacing first, then segment
        // size, then segment COUNT — never overflow, never render nothing.
        var availableMain = IsVertical ? RootGrid.ActualHeight : RootGrid.ActualWidth;
        double spacing = 2;
        double segMain = IsVertical ? 7 : 4;
        if (availableMain > 0)
        {
            if ((segMain + spacing) * count > availableMain)
            {
                spacing = 1;
            }
            var perSegment = (availableMain - spacing * (count - 1)) / count;
            if (perSegment < segMain)
            {
                segMain = Math.Max(2, Math.Floor(perSegment));
            }
            var fits = (int)Math.Floor((availableMain + spacing) / (segMain + spacing));
            if (fits < count && fits >= 4)
            {
                // Too small even at 2px segments: re-quantize onto fewer segments
                // so the meter stays proportional instead of clipping.
                count = fits;
                activeSegments = (int)Math.Round(level / 100.0 * count, MidpointRounding.AwayFromZero);
                peakSegment = _peakLevel > 0
                    ? (int)Math.Round(Math.Clamp(_peakLevel, 0, 100) / 100.0 * count, MidpointRounding.AwayFromZero) - 1
                    : -1;
            }
        }

        var panel = new StackPanel
        {
            Orientation = IsVertical ? Orientation.Vertical : Orientation.Horizontal,
            HorizontalAlignment = IsVertical ? HorizontalAlignment.Center : HorizontalAlignment.Stretch,
            VerticalAlignment = IsVertical ? VerticalAlignment.Bottom : VerticalAlignment.Center,
            Spacing = spacing
        };

        for (var visualIndex = 0; visualIndex < count; visualIndex++)
        {
            var lowToHighIndex = IsVertical ? count - visualIndex - 1 : visualIndex;
            var isActive = lowToHighIndex < activeSegments;
            var isPeakHold = lowToHighIndex == peakSegment && !isActive;
            var normalized = (lowToHighIndex + 1) / (double)count;

            panel.Children.Add(new Border
            {
                Width = IsVertical ? 14 : segMain,
                Height = IsVertical ? segMain : 10,
                CornerRadius = new CornerRadius(1.5),
                Background = isActive || isPeakHold ? BrushFor(normalized) : DimBrush
            });
        }

        RootGrid.Children.Clear();
        RootGrid.Children.Add(panel);
    }

    private static SolidColorBrush BrushFor(double normalized) =>
        normalized >= 0.9 ? RedBrush : normalized >= 0.7 ? YellowBrush : GreenBrush;
}
