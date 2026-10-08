using System.Diagnostics;
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
    private readonly Timer _timer;
    private Draft _draft;
    private Task _inFlight = Task.CompletedTask;
    private volatile bool _disposed;
    private long _lastRevision = -1, _lastSent;
    private sealed record Draft(long Revision, ColorGrade Grade);
    public GradePreviewCoordinator(IGradePreviewTransport bridge, ColorGradeEditorViewModel editor, Action<Action> dispatch)
    {
        _bridge = bridge; _editor = editor; _dispatch = dispatch;
        _draft = new(editor.Revision, editor.PreviewGrade);
        editor.PreviewRequested += OnDraftChanged;
        bridge.GradePreviewReceived += OnObservation;
        bridge.HealthChanged += OnHealth;
        _timer = new Timer(_ => Tick(), null, 0, 100);
    }
    private void OnDraftChanged(object? sender, EventArgs e)
    {
        lock (_gate) _draft = new(_editor.Revision, _editor.PreviewGrade);
    }
    private void Tick()
    {
        lock (_gate)
        {
            if (_disposed || !_inFlight.IsCompleted) return;
            if (_lastRevision == _draft.Revision && Stopwatch.GetElapsedTime(_lastSent).TotalMilliseconds < 500) return;
            _lastRevision = _draft.Revision; _lastSent = Stopwatch.GetTimestamp();
            _inFlight = SendAsync(_draft, true);
        }
    }
    private async Task SendAsync(Draft draft, bool enabled)
    {
        try { await _bridge.SetGradePreviewAsync(_editor.InstanceId, _editor.SourceId, draft.Revision, enabled,
            new(draft.Grade.Lut, draft.Grade.Exposure, draft.Grade.Contrast, draft.Grade.Saturation,
                draft.Grade.Temperature)).ConfigureAwait(false); }
        catch (Exception ex)
        {
            if (enabled) _dispatch(() => { if (!_disposed && _editor.Revision == draft.Revision) _editor.SetNativeUnavailable(ex.Message); });
        }
    }
    private void OnObservation(GradePreviewObservation observation)
    {
        if (observation.InstanceId != _editor.InstanceId) return;
        _dispatch(() => { if (!_disposed) _editor.ObserveNativePreview(observation); });
    }
    private void OnHealth(MediaCoreHealth health)
    {
        if (health.Recovering || health.Stopped)
            _dispatch(() => { if (!_disposed) _editor.SetNativeUnavailable("Native preview reconnecting."); });
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
