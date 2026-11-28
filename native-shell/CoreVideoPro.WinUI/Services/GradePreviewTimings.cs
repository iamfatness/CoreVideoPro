using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.WinUI.Services;

/// <summary>Designer preview timing seen by the shell, per draft revision: edit captured, command sent,
/// command acknowledged, first native frame (ready texture) observed. Bounded (the newest
/// <see cref="Capacity"/> revisions) and lock-protected; it never touches pixels or the UI.
/// The headless <see cref="DesignerPreviewProbe"/> reads it; presentation on screen is not measured here.</summary>
internal sealed class GradePreviewTimings
{
    internal const int Capacity = 1024;
    internal sealed record Revision(long Number, long EditedAt, long SentAt, long AckedAt, long ReadyAt, GradePreviewTiming? Core);
    internal sealed record Snapshot(long Edits, long Sends, long Renewals, long Failures, IReadOnlyList<Revision> Revisions, long Frequency);

    private readonly object _gate = new();
    private readonly TimeProvider _time;
    private readonly Dictionary<long, Revision> _revisions = [];
    private readonly Queue<long> _order = new();
    private long _edits, _sends, _renewals, _failures;

    internal GradePreviewTimings(TimeProvider time) => _time = time;

    private Revision Row(long revision)
    {
        if (_revisions.TryGetValue(revision, out var row)) return row;
        row = new(revision, 0, 0, 0, 0, null);
        _revisions[revision] = row; _order.Enqueue(revision);
        while (_order.Count > Capacity) _revisions.Remove(_order.Dequeue());
        return row;
    }
    internal void Edited(long revision)
    {
        lock (_gate) { ++_edits; var row = Row(revision); if (row.EditedAt == 0) _revisions[revision] = row with { EditedAt = _time.GetTimestamp() }; }
    }
    internal void Sent(long revision, bool renew)
    {
        lock (_gate) {
            if (renew) { ++_renewals; return; }
            ++_sends; var row = Row(revision); if (row.SentAt == 0) _revisions[revision] = row with { SentAt = _time.GetTimestamp() };
        }
    }
    internal void Acked(long revision)
    {
        lock (_gate) { if (_revisions.TryGetValue(revision, out var row) && row.AckedAt == 0) _revisions[revision] = row with { AckedAt = _time.GetTimestamp() }; }
    }
    internal void Failed() { lock (_gate) ++_failures; }
    internal void Observed(GradePreviewObservation observation)
    {
        if (observation.Status != "ready" || observation.Texture is not { Width: > 0 }) return;
        lock (_gate) {
            if (_revisions.TryGetValue(observation.Revision, out var row) && row.ReadyAt == 0)
                _revisions[observation.Revision] = row with { ReadyAt = _time.GetTimestamp(), Core = observation.Timing };
        }
    }
    internal Snapshot Read()
    {
        lock (_gate) return new(_edits, _sends, _renewals, _failures, _order.Select(r => _revisions[r]).ToArray(), _time.TimestampFrequency);
    }
    internal void Reset()
    {
        lock (_gate) { _revisions.Clear(); _order.Clear(); _edits = _sends = _renewals = _failures = 0; }
    }
}
