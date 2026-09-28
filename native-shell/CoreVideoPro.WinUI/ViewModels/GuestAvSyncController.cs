using CommunityToolkit.Mvvm.ComponentModel;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>
/// Session-scoped operator intent for one Zoom guest at a time. Core snapshot
/// facts repair command-response gaps; PCM and pixels remain entirely native.
/// </summary>
public sealed class GuestAvSyncController : ObservableObject
{
    private readonly Func<string, int, Task<NativeZoomGuestAvSyncAck>> _send;
    private readonly Func<string?> _selectedParticipantId;
    private readonly Func<bool> _perGuestIso;
    private readonly Func<bool> _zoomLive;
    private readonly SemaphoreSlim _sendGate = new(1, 1);
    private readonly Dictionary<string, int> _applied = new(StringComparer.Ordinal);
    private long _sessionGeneration;
    private string _status = string.Empty;
    private long _lastRevision = -1;

    public GuestAvSyncController(IMediaCoreBridge bridge,
        Func<string?> selectedParticipantId, Func<bool> perGuestIso,
        Func<bool>? zoomLive = null)
        : this((id, offset) => bridge.SetZoomGuestAvSyncOffsetAsync(id, offset),
            selectedParticipantId, perGuestIso, zoomLive)
    {
    }

    public GuestAvSyncController(Func<string, int, Task<NativeZoomGuestAvSyncAck>> send,
        Func<string?> selectedParticipantId, Func<bool> perGuestIso,
        Func<bool>? zoomLive = null)
    {
        _send = send;
        _selectedParticipantId = selectedParticipantId;
        _perGuestIso = perGuestIso;
        _zoomLive = zoomLive ?? (() => true);
    }

    private string? SelectedZoomId
    {
        get
        {
            var id = _selectedParticipantId();
            return !string.IsNullOrWhiteSpace(id) && !id.Contains(':') && id != "zoom-mix"
                ? id : null;
        }
    }

    public bool CanAdjust => _zoomLive() && _perGuestIso() && SelectedZoomId is not null;
    public int OffsetMs => SelectedZoomId is { } id && _applied.TryGetValue(id, out var value)
        ? value : 0;
    public string Label => !CanAdjust ? "Guest sync unavailable" : OffsetMs switch
    {
        > 0 => $"Audio +{OffsetMs} ms later",
        < 0 => $"Video {-OffsetMs} ms later",
        _ => "Lip sync 0 ms"
    };
    public string Explanation => !_zoomLive()
        ? "Join a Zoom meeting before adjusting a source."
        : !_perGuestIso()
        ? "Per-guest ISO audio is required; Zoom program mix cannot isolate a guest."
        : SelectedZoomId is null ? "Select a Zoom guest channel to adjust lip sync."
        : "Positive delays this guest's audio. Negative delays this guest's video.";
    public string Status => _status;

    public void Refresh()
    {
        OnPropertyChanged(nameof(CanAdjust));
        OnPropertyChanged(nameof(OffsetMs));
        OnPropertyChanged(nameof(Label));
        OnPropertyChanged(nameof(Explanation));
        OnPropertyChanged(nameof(Status));
    }

    public void ClearSession()
    {
        _sessionGeneration++;
        _applied.Clear();
        _lastRevision = -1;
        _status = string.Empty;
        Refresh();
    }

    public void ApplySnapshot(long revision, IReadOnlyList<NativeZoomGuestAvSyncSetting> settings)
    {
        if (revision < _lastRevision) return;
        _lastRevision = revision;
        var selected = SelectedZoomId;
        var prior = OffsetMs;
        var next = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var item in settings)
        {
            if (!string.IsNullOrWhiteSpace(item.ParticipantId))
                next[item.ParticipantId] = Math.Clamp(item.OffsetMs, -200, 200);
        }
        _applied.Clear();
        foreach (var item in next) _applied[item.Key] = item.Value;
        if (selected is not null && prior != OffsetMs) Refresh();
    }

    public Task NudgeEarlierAsync() => ChangeAsync(-10);
    public Task NudgeLaterAsync() => ChangeAsync(10);
    public Task ResetAsync() => SetAsync(0);
    public Task SetAsync(int offsetMs) => ChangeAsync(0, Math.Clamp(offsetMs, -200, 200));

    private async Task ChangeAsync(int delta, int? absolute = null)
    {
        if (!CanAdjust || SelectedZoomId is not { } id) return;
        await _sendGate.WaitAsync();
        try
        {
            var generation = _sessionGeneration;
            var current = _applied.GetValueOrDefault(id);
            var requested = Math.Clamp(absolute ?? current + delta, -200, 200);
            var ack = await _send(id, requested);
            if (generation != _sessionGeneration) return;
            if (ack.Revision < _lastRevision) return;
            _lastRevision = ack.Revision;
            if (ack.OffsetMs == 0) _applied.Remove(id);
            else _applied[id] = ack.OffsetMs;
            _status = ack.OffsetMs switch
            {
                > 0 => $"{id}: audio delayed {ack.OffsetMs} ms",
                < 0 => $"{id}: video delayed {-ack.OffsetMs} ms",
                _ => $"{id}: lip sync reset"
            };
        }
        catch (Exception error)
        {
            _status = $"Lip sync not applied: {error.Message}";
        }
        finally
        {
            _sendGate.Release();
            Refresh();
        }
    }
}
