using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;

namespace CoreVideoPro.WinUI.Services;

/// <summary>One leased native monitor demand behind a latest-value mailbox. No pixels or UI work on timers.
/// An edit is sent as soon as no command is in flight and one display frame has passed since the last send;
/// edits that arrive meanwhile only replace the pending draft, so a burst sends its newest revision and never
/// a stale one. There is no wait for a native frame: the core keeps one draft revision in its render pipeline
/// and holds the newest waiting one (GradePreviewController), and
/// <see cref="ColorGradeEditorViewModel.ObserveNativePreview"/> accepts only forward progress, so frames never
/// go backwards. An unchanged draft renews its three-second native lease every 500 ms; between renewals the
/// one-shot timer sleeps, so a parked editor costs nothing.</summary>
public sealed class GradePreviewCoordinator : IDisposable
{
    internal static readonly TimeSpan FrameInterval = TimeSpan.FromMilliseconds(16);
    internal static readonly TimeSpan RenewInterval = TimeSpan.FromMilliseconds(500);
    internal static readonly TimeSpan RetryInterval = TimeSpan.FromMilliseconds(100);
    private readonly IGradePreviewTransport _bridge;
    private readonly ColorGradeEditorViewModel _editor;
    private readonly Action<Action> _dispatch;
    private readonly object _gate = new();
    private readonly TimeProvider _time;
    private readonly ITimer _timer;
    private Draft _draft;
    private Task _inFlight = Task.CompletedTask;
    private volatile bool _disposed;
    private bool _sending, _sentOnce, _retrying;
    private long _lastRevision = -1, _lastSent;
    internal GradePreviewTimings Timings { get; }
    private sealed record Draft(long Revision, ColorGrade Grade, bool ScopesEnabled, bool ScopesOriginal, int HistogramMode, int WaveformMode, bool CompareOriginal, int ScopeView, GradeScopeRoi Roi, LowerThirdPreview? LowerThirdPreview);
    public GradePreviewCoordinator(IGradePreviewTransport bridge, ColorGradeEditorViewModel editor, Action<Action> dispatch)
        : this(bridge, editor, dispatch, TimeProvider.System) { }

    internal GradePreviewCoordinator(IGradePreviewTransport bridge, ColorGradeEditorViewModel editor,
        Action<Action> dispatch, TimeProvider time)
    {
        _bridge = bridge; _editor = editor; _dispatch = dispatch; _time = time;
        Timings = new(time);
        _draft = CaptureDraft();
        editor.PreviewRequested += OnDraftChanged;
        bridge.GradePreviewReceived += OnObservation;
        bridge.HealthChanged += OnHealth;
        _timer = _time.CreateTimer(_ => Tick(), null, TimeSpan.Zero, Timeout.InfiniteTimeSpan);
    }
    private void OnDraftChanged(object? sender, EventArgs e)
    {
        lock (_gate)
        {
            _draft = CaptureDraft();
            // While a command is in flight its completion decides; otherwise wake for this draft.
            if (!_disposed && !_sending) Schedule(Wait());
        }
        Timings.Edited(_editor.Revision);
    }
    private Draft CaptureDraft() => new(_editor.Revision, _editor.CurrentGrade, _editor.AdvancedExpanded && _editor.ScopesEnabled,
        _editor.ScopesOriginal, _editor.HistogramMode, _editor.WaveformMode, _editor.CompareOriginal, _editor.ScopeView, _editor.ScopeRoi, _editor.LowerThirdPreview);
    // Under _gate: how long until the mailbox may send. A new draft waits out the rest of one display frame
    // (a refused one, the retry interval); an unchanged draft waits for its lease renewal.
    private TimeSpan Wait()
    {
        if (!_sentOnce) return TimeSpan.Zero;
        var interval = _lastRevision == _draft.Revision ? RenewInterval : _retrying ? RetryInterval : FrameInterval;
        var since = _time.GetElapsedTime(_lastSent);
        return interval > since ? interval - since : TimeSpan.Zero;
    }
    private void Schedule(TimeSpan due) => _timer.Change(due, Timeout.InfiniteTimeSpan);
    private void Tick()
    {
        lock (_gate)
        {
            if (_disposed || _sending) return;
            var wait = Wait();
            if (wait > TimeSpan.Zero) { Schedule(wait); return; }
            var renew = _lastRevision == _draft.Revision;
            _lastRevision = _draft.Revision; _lastSent = _time.GetTimestamp(); _sentOnce = true; _sending = true;
            _inFlight = SendThenContinueAsync(_draft, renew);
        }
    }
    private async Task SendThenContinueAsync(Draft draft, bool renew)
    {
        var sent = await SendAsync(draft, true, renew).ConfigureAwait(false);
        lock (_gate)
        {
            _sending = false; _retrying = !sent;
            if (!_disposed) Schedule(Wait()); // the newest draft, the next renewal or a retry
        }
    }
    private async Task<bool> SendAsync(Draft draft, bool enabled, bool renew = false)
    {
        try {
            var wire = new MediaCoreColorGradeWire(draft.Grade.Lut, draft.Grade.Exposure, draft.Grade.Contrast, draft.Grade.Saturation,
                draft.Grade.Temperature, draft.Grade.Advanced?.Copy(), draft.ScopesEnabled, draft.ScopesOriginal,
                draft.HistogramMode, draft.WaveformMode, draft.CompareOriginal, draft.ScopeView, draft.Roi, draft.LowerThirdPreview);
            Timings.Sent(draft.Revision, renew);
            if (renew) await _bridge.RenewGradePreviewAsync(_editor.InstanceId,_editor.SourceId,draft.Revision,wire).ConfigureAwait(false);
            else await _bridge.SetGradePreviewAsync(_editor.InstanceId,_editor.SourceId,draft.Revision,enabled,wire).ConfigureAwait(false);
            if (!renew) Timings.Acked(draft.Revision);
            return true;
        }
        catch (Exception ex)
        {
            Timings.Failed();
            lock (_gate) { if (_lastRevision == draft.Revision) _lastRevision = -1; }
            if (enabled) _dispatch(() => { if (!_disposed && _editor.Revision == draft.Revision) _editor.SetNativeUnavailable(ex.Message); });
            return false;
        }
    }
    private void OnObservation(GradePreviewObservation observation)
    {
        if (observation.InstanceId != _editor.InstanceId || observation.SourceId != _editor.SourceId) return;
        Timings.Observed(observation);
        _dispatch(() => { if (!_disposed) _editor.ObserveNativePreview(observation); });
    }
    private void OnHealth(MediaCoreHealth health)
    {
        if (health.Recovering || health.Stopped)
        {
            lock (_gate) { _lastRevision = -1; if (!_disposed && !_sending) Schedule(Wait()); }
            _dispatch(() => { if (!_disposed) { _editor.ResetGradeAuthority(); _editor.SetNativeUnavailable("Native preview reconnecting."); } });
        }
    }
    public void Dispose()
    {
        Task pending; Draft draft;
        lock (_gate) { if (_disposed) return; _disposed = true; pending = _inFlight; draft = _draft; }
        _timer.Dispose();
        _editor.PreviewRequested -= OnDraftChanged;
        _bridge.GradePreviewReceived -= OnObservation;
        _bridge.HealthChanged -= OnHealth;
        _ = CloseAfterAsync(pending, draft);
    }
    private async Task CloseAfterAsync(Task pending, Draft draft)
    {
        await pending.ConfigureAwait(false);
        await SendAsync(draft, false).ConfigureAwait(false);
    }
}
