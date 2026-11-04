using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI.ViewModels;

/// <summary>Owns remote feeds for the lifetime of the show workspace.
/// Timer and persistence callbacks run on the same dispatcher as other graphics edits.</summary>
internal sealed class GraphicsFeedViewModel : IDisposable
{
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly GraphicsPlayoutService _playout;
    private readonly DispatcherQueueTimer _timer;
    private readonly GraphicsFeedCredentialStore _credentials = new();
    private readonly GraphicsFeedTransport _transport;
    private readonly Dictionary<string, GraphicsFeedPoller> _pollers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, GraphicsPushFeedCoordinator> _pushFeeds = new(StringComparer.Ordinal);
    private Dictionary<string, GraphicsFeedStatus> _statuses = new(StringComparer.Ordinal);
    private bool _disposed;

    internal GraphicsFeedViewModel(GraphicsWorkspaceLibrary library, GraphicsPlayoutService playout, DispatcherQueue dispatcher)
    {
        (_library, _playout) = (library, playout);
        _transport = GraphicsFeedTransport.Create(ResolveCredential);
        _timer = dispatcher.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += OnTick;
        _library.Changed += OnLibraryChanged;
        Synchronize();
    }

    internal event EventHandler? Changed;

    private string? ResolveCredential(GraphicsFeedDefinition definition) => _credentials.Resolve(_library.Document.ShowId, definition);

    internal Task<GraphicsFeedDefinition?> SaveFeedAsync(GraphicsFeedDefinition definition, string? replacementBearer = null,
        bool clearCredential = false) => _credentials.SaveAsync(_library, definition, replacementBearer, clearCredential);

    internal GraphicsFeedStatus Status(string id) => _pushFeeds.TryGetValue(id, out var push) ? push.Status :
        _pollers.TryGetValue(id, out var poller) ? poller.Status : new();

    internal void Start()
    {
        _ = _library.LoadAsync();
        _timer.Start();
    }

    internal void Refresh(string id)
    {
        if (!_timer.IsRunning) return;
        if (_pushFeeds.TryGetValue(id, out var push))
        {
            push.ReconnectNow();
            push.Tick();
        }
        else if (_pollers.TryGetValue(id, out var poller))
        {
            poller.RefreshNow();
            Poll(poller);
        }
        PublishStatus();
    }

    private void OnLibraryChanged(object? sender, EventArgs args) => Synchronize();

    private void Synchronize()
    {
        if (_disposed) return;
        var pullFeeds = _library.Document.Feeds.Where(feed => feed.Format != "wss").ToArray();
        foreach (var key in _pollers.Keys.Where(id => !pullFeeds.Any(feed => feed.Id == id)).ToArray())
        {
            _pollers[key].Dispose();
            _pollers.Remove(key);
        }
        foreach (var feed in pullFeeds)
        {
            if (!_pollers.TryGetValue(feed.Id, out var poller))
            {
                poller = new(_transport.FetchAsync, (definition, data, cancellation) =>
                    _playout.StageFeedAsync(definition, data, DateTimeOffset.UtcNow, cancellation), TimeProvider.System);
                _pollers.Add(feed.Id, poller);
            }
            poller.Configure(feed);
        }
        var pushFeeds = _library.Document.Feeds.Where(feed => feed.Format == "wss").ToArray();
        foreach (var key in _pushFeeds.Keys.Where(id => !pushFeeds.Any(feed => feed.Id == id)).ToArray())
        {
            _pushFeeds[key].Dispose();
            _pushFeeds.Remove(key);
        }
        foreach (var feed in pushFeeds)
        {
            if (!_pushFeeds.TryGetValue(feed.Id, out var push))
            {
                push = new(_playout.StagePushSnapshotAsync, TimeProvider.System, ResolveCredential);
                _pushFeeds.Add(feed.Id, push);
            }
            push.Configure(feed, _library.Document.DataSources.FirstOrDefault(source => source.Id == feed.Id));
        }
        PublishStatus();
    }

    private void OnTick(DispatcherQueueTimer sender, object args)
    {
        foreach (var poller in _pollers.Values.ToArray()) Poll(poller);
        foreach (var push in _pushFeeds.Values.ToArray()) push.Tick();
        PublishStatus();
    }

    private void Poll(GraphicsFeedPoller poller)
    {
        var task = poller.TickAsync();
        if (!task.IsCompletedSuccessfully) _ = Complete(task);
    }

    private async Task Complete(Task task)
    {
        await task;
        if (!_disposed) PublishStatus();
    }

    private void PublishStatus()
    {
        var next = _pollers.Keys.Concat(_pushFeeds.Keys).ToDictionary(id => id, Status, StringComparer.Ordinal);
        if (next.Count == _statuses.Count && next.All(pair => _statuses.GetValueOrDefault(pair.Key) == pair.Value)) return;
        _statuses = next;
        Changed?.Invoke(this, EventArgs.Empty);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _timer.Stop();
        _timer.Tick -= OnTick;
        _library.Changed -= OnLibraryChanged;
        foreach (var poller in _pollers.Values) poller.Dispose();
        _pollers.Clear();
        foreach (var push in _pushFeeds.Values) push.Dispose();
        _pushFeeds.Clear();
        _transport.Dispose();
    }
}

public sealed partial class StudioViewModel
{
    private GraphicsFeedViewModel? _graphicsFeeds;
    internal GraphicsFeedViewModel GraphicsFeeds => _graphicsFeeds ??= new(GraphicsLibrary, GraphicsPlayout, _dispatcher);
}
