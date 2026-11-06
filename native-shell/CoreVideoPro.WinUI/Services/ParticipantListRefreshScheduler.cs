namespace CoreVideoPro.WinUI.Services;

/// <summary>UI-thread-only two-turn refresh, invalidated before a workspace unloads.</summary>
internal sealed class ParticipantListRefreshScheduler(
    Action<Action, string> enqueue, Action clear, Action rebind)
{
    private long _generation;
    private long _revision;
    private bool _active;
    private bool _pending;

    public void Activate()
    {
        if (_active) return;
        _active = true;
        _generation++;
        _pending = false;
    }

    public void Deactivate()
    {
        _active = false;
        _generation++;
        _pending = false;
    }

    public void Schedule()
    {
        if (!_active) return;
        _revision++;
        if (_pending) return;
        _pending = true;
        var generation = _generation;
        try { enqueue(() => Clear(generation), "StudioWorkspace.participant-list.clear"); }
        catch { Reset(generation); throw; }
    }

    private bool Current(long generation) => _active && generation == _generation;

    private void Reset(long generation)
    {
        if (Current(generation)) _pending = false;
    }

    private void Clear(long generation)
    {
        if (!Current(generation)) return;
        try
        {
            clear();
            // Clearing can itself raise events, including unloading this workspace.
            if (Current(generation))
                enqueue(() => Rebind(generation), "StudioWorkspace.participant-list.rebind");
        }
        catch { Reset(generation); throw; }
    }

    private void Rebind(long generation)
    {
        if (!Current(generation)) return;
        var revision = _revision;
        try { rebind(); }
        finally { Reset(generation); }
        // A roster notification raised during binding must not be lost.
        if (Current(generation) && revision != _revision) Schedule();
    }
}
