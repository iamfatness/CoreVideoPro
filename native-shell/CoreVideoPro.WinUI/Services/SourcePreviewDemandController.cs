namespace CoreVideoPro.WinUI.Services;

/// <summary>One bounded in-flight control and a short renewable native consumer lease.</summary>
public sealed class SourcePreviewDemandController(Func<bool, Task> send)
{
    private readonly object _gate = new();
    private Timer? _renewal;
    private bool _visible, _engineAvailable = true;
    private int _version, _sending;

    public Task SetVisibleAsync(bool visible)
    {
        lock (_gate)
        {
            if (_visible == visible) return Task.CompletedTask;
            _visible = visible;
            UpdateRenewal();
        }
        return RefreshAsync();
    }

    public Task SetEngineAvailableAsync(bool available)
    {
        lock (_gate)
        {
            if (_engineAvailable == available) return Task.CompletedTask;
            _engineAvailable = available;
            UpdateRenewal();
        }
        return RefreshAsync();
    }

    private void UpdateRenewal()
    {
        _version++;
        if (_visible && _engineAvailable)
            _renewal ??= new Timer(state => { _ = RefreshAsync(); }, null, 1000, 1000);
        else
        {
            _renewal?.Dispose();
            _renewal = null;
        }
    }

    public async Task RefreshAsync()
    {
        if (Interlocked.Exchange(ref _sending, 1) != 0) return;
        var sentVersion = -1;
        try
        {
            while (true)
            {
                bool active;
                lock (_gate) { sentVersion = _version; active = _visible && _engineAvailable; }
                await send(active).ConfigureAwait(false);
                lock (_gate) if (sentVersion == _version) break;
            }
        }
        catch (Exception)
        {
            // A stopped/recovering core cannot acknowledge. A live visible page
            // renews after recovery; an abandoned native lease expires by itself.
        }
        finally
        {
            Interlocked.Exchange(ref _sending, 0);
            bool changed;
            lock (_gate) changed = sentVersion != _version;
            if (changed) _ = RefreshAsync();
        }
    }
}
