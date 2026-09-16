namespace CoreVideoPro.WinUI.Models;

/// <summary>Meter state driven by monotonic milliseconds; owns no UI objects or timers.</summary>
public sealed class AudioMeterBallistics
{
    private double _target;
    private long _lastTick;
    private long _peakAt;
    public double Level { get; private set; }
    public double Peak { get; private set; }
    public bool NeedsAnimation => Level > _target || Peak > _target;

    public void Reset(double level, bool muted, bool showInput, long now)
    {
        Level = Peak = _target = Target(level, muted, showInput);
        _lastTick = _peakAt = now;
    }

    public void SetInput(double level, bool muted, bool showInput, long now)
    {
        // Account for elapsed time using the previous target before accepting
        // the new sample. Snapshot rate must not change the release envelope.
        Advance(now);
        _target = Target(level, muted, showInput);
        if (muted && !showInput)
        {
            Reset(0, true, false, now);
            return;
        }
        Level = Math.Max(Level, _target);
        if (_target >= Peak)
        {
            Peak = _target;
            _peakAt = now;
        }
    }

    public void Advance(long now)
    {
        var elapsed = Math.Max(0, now - _lastTick);
        if (Level > _target)
            Level = Math.Max(_target, Level * Math.Exp(-elapsed / 300.0) - 0.25 * elapsed / 33.0);
        var peakElapsed = Math.Max(0, now - Math.Max(_lastTick, _peakAt + 800));
        if (Peak > Level && peakElapsed > 0)
            Peak = Math.Max(Level, Peak - 4.0 * peakElapsed / 33.0);
        _lastTick = now;
    }

    private static double Target(double level, bool muted, bool showInput) =>
        (muted && !showInput) || !double.IsFinite(level) ? 0 : Math.Clamp(level, 0, 100);
}
