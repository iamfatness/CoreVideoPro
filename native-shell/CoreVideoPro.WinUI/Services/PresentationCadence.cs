namespace CoreVideoPro.WinUI.Services;

/// <summary>Accumulate deadlines across display ticks; do not round every interval to milliseconds.</summary>
internal sealed class PresentationCadence
{
    private long _next;
    private long _retryAt;
    internal bool TrySubmit(long now, long frequency, int fps, bool retryPending, out bool retry)
    {
        retry = false;
        if (ShouldSubmit(now, frequency, fps))
        {
            _retryAt = now + Math.Max(1, frequency / 1000);
            return true;
        }
        if (!retryPending || now < _retryAt) return false;
        // Retry only inside the original slot. A missed slot is never extended
        // or replayed; the next periodic submission always takes precedence.
        _retryAt = now + Math.Max(1, frequency / 1000);
        retry = true;
        return true;
    }
    internal long Delay100ns(long now, long frequency, bool retryPending) =>
        Math.Max(1, (long)Math.Ceiling(((retryPending ? Math.Min(_next, _retryAt) : _next) - now)
            * (10_000_000d / frequency)));
    internal long Delay100ns(long now, long frequency) =>
        Math.Max(1, (long)Math.Ceiling((_next - now) * (10_000_000d / frequency)));
    internal bool ShouldSubmit(long now, long frequency, int fps)
    {
        if (now < _next) return false;
        var interval = frequency / fps;
        _next = _next == 0 || now - _next >= interval ? now + interval : _next + interval;
        return true;
    }
}
