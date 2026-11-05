using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.ViewModels;

namespace CoreVideoPro.WinUI.Services;

/// <summary>One leased, coalesced native monitor demand. No pixels or UI work on timers.</summary>
public sealed class GradePreviewCoordinator : IDisposable
{
    private readonly IGradePreviewTransport _bridge;
    private readonly ColorGradeEditorViewModel _editor;
    private readonly Action<Action> _dispatch;
    private readonly object _gate = new();
    private readonly TimeProvider _time;
    private readonly ITimer _timer;
    private Draft _draft;
    private Task _inFlight = Task.CompletedTask;
    private volatile bool _disposed;
    private long _lastRevision = -1, _lastSent, _completedRevision = -1;
    private sealed record Draft(long Revision, ColorGrade Grade, bool ScopesEnabled, bool ScopesOriginal, int HistogramMode, int WaveformMode, bool CompareOriginal, int ScopeView, GradeScopeRoi Roi, LowerThirdPreview? LowerThirdPreview, bool ContinuousGraphics);
    public GradePreviewCoordinator(IGradePreviewTransport bridge, ColorGradeEditorViewModel editor, Action<Action> dispatch)
        : this(bridge, editor, dispatch, TimeProvider.System) { }

    internal GradePreviewCoordinator(IGradePreviewTransport bridge, ColorGradeEditorViewModel editor,
        Action<Action> dispatch, TimeProvider time)
    {
        _bridge = bridge; _editor = editor; _dispatch = dispatch; _time = time;
        _draft = CaptureDraft();
        editor.PreviewRequested += OnDraftChanged;
        bridge.GradePreviewReceived += OnObservation;
        bridge.HealthChanged += OnHealth;
        _timer = _time.CreateTimer(_ => Tick(), null, TimeSpan.Zero, TimeSpan.FromMilliseconds(100));
    }
    private void OnDraftChanged(object? sender, EventArgs e)
    {
        lock (_gate) _draft = CaptureDraft();
    }
    private Draft CaptureDraft() => new(_editor.Revision, _editor.CurrentGrade, _editor.AdvancedExpanded && _editor.ScopesEnabled,
        _editor.ScopesOriginal, _editor.HistogramMode, _editor.WaveformMode, _editor.CompareOriginal, _editor.ScopeView, _editor.ScopeRoi, _editor.LowerThirdPreview, _editor.ContinuousGraphicsPreview);
    private void Tick()
    {
        lock (_gate)
        {
            if (_disposed || !_inFlight.IsCompleted) return;
            var renew = _lastRevision == _draft.Revision;
            var sinceSend = _time.GetElapsedTime(_lastSent);
            if (renew && sinceSend.TotalMilliseconds < 500) return;
            // Native publishes only its admitted revision. Let that revision produce a frame
            // before replacing it with the newest pointer edit. Bound the wait on failure.
            if (!renew && _draft.ContinuousGraphics && _lastRevision > _completedRevision &&
                sinceSend.TotalMilliseconds < 500) return;
            _lastRevision = _draft.Revision; _lastSent = _time.GetTimestamp();
            _inFlight = SendAsync(_draft, true, renew);
        }
    }
    private async Task SendAsync(Draft draft, bool enabled, bool renew = false)
    {
        try {
            var wire = new MediaCoreColorGradeWire(draft.Grade.Lut, draft.Grade.Exposure, draft.Grade.Contrast, draft.Grade.Saturation,
                draft.Grade.Temperature, draft.Grade.Advanced?.Copy(), draft.ScopesEnabled, draft.ScopesOriginal,
                draft.HistogramMode, draft.WaveformMode, draft.CompareOriginal, draft.ScopeView, draft.Roi, draft.LowerThirdPreview);
            if (renew) await _bridge.RenewGradePreviewAsync(_editor.InstanceId,_editor.SourceId,draft.Revision,wire).ConfigureAwait(false);
            else await _bridge.SetGradePreviewAsync(_editor.InstanceId,_editor.SourceId,draft.Revision,enabled,wire).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            lock (_gate) { if (_lastRevision == draft.Revision) _lastRevision = -1; }
            if (enabled) _dispatch(() => { if (!_disposed && _editor.Revision == draft.Revision) _editor.SetNativeUnavailable(ex.Message); });
        }
    }
    private void OnObservation(GradePreviewObservation observation)
    {
        if (observation.InstanceId != _editor.InstanceId || observation.SourceId != _editor.SourceId) return;
        lock (_gate)
        {
            if (observation.Revision == _lastRevision &&
                (observation.Status == "unavailable" || (observation.Status == "ready" && observation.Texture is not null)))
                _completedRevision = observation.Revision;
        }
        _dispatch(() => { if (!_disposed) _editor.ObserveNativePreview(observation); });
    }
    private void OnHealth(MediaCoreHealth health)
    {
        if (health.Recovering || health.Stopped)
        {
            lock (_gate) _lastRevision = _completedRevision = -1;
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
