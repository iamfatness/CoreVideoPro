namespace CoreVideoPro.WinUI.Services;

/// <summary>Accumulate deadlines across display ticks; do not round every interval to milliseconds.</summary>
internal sealed class PresentationCadence
{
    private long _next;
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
