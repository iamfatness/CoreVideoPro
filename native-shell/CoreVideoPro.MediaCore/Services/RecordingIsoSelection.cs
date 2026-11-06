namespace CoreVideoPro.MediaCore.Services;

/// <summary>Retains an active take's selected identities across passive source loss.
/// Availability admits a writer; it does not revoke one already in the take.</summary>
public sealed class RecordingIsoSelection
{
    private readonly object _gate = new();
    private IReadOnlyList<string> _active = [];
    private HashSet<string>? _selected;
    private HashSet<string>? _assigned;
    private bool _refreshIntent;

    public void Reset()
    {
        lock (_gate)
        {
            _active = [];
            _selected = null;
            _assigned = null;
            _refreshIntent = false;
        }
    }

    public void RefreshIntent() { lock (_gate) _refreshIntent = true; }

    public IReadOnlyList<string> Resolve(bool recordingRequested, bool enabled,
        IEnumerable<string> selected, IEnumerable<string> eligible, IEnumerable<string> assigned)
    {
        lock (_gate)
        {
            if (!recordingRequested || !enabled)
            {
                Reset();
                return IsoSourceSelectionResolver.Resolve(enabled, selected, eligible);
            }
            var selectedSet = selected.ToHashSet(StringComparer.Ordinal);
            // Assigning an unrelated, unselected source is not an ISO edit.
            var assignedSet = assigned.Where(selectedSet.Contains).ToHashSet(StringComparer.Ordinal);
            if (!_refreshIntent && _selected is not null && _selected.SetEquals(selectedSet)
                && _assigned is not null && _assigned.SetEquals(assignedSet)) return _active;
            var candidates = _active.Where(assignedSet.Contains).Concat(eligible);
            _active = IsoSourceSelectionResolver.Resolve(enabled, selectedSet, candidates);
            _selected = selectedSet;
            _assigned = assignedSet;
            _refreshIntent = false;
            return _active;
        }
    }
}
