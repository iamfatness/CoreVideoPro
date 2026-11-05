using System.Security.Cryptography;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Microsoft.UI.Dispatching;

namespace CoreVideoPro.WinUI.ViewModels;

internal sealed class GraphicsSocialReviewViewModel
{
    private readonly GraphicsWorkspaceLibrary _library;
    private readonly DispatcherQueue _dispatcher;
    private readonly DispatcherQueueTimer _timer;
    private readonly YouTubeChatCredentialStore _credentials = new();
    private CancellationTokenSource? _connection;
    private Task _feed = Task.CompletedTask;
    private string _showId;
    private int _generation;
    private bool _sweeping, _closed;

    public GraphicsSocialReviewViewModel(GraphicsWorkspaceLibrary library, GraphicsSocialPlayout playout, DispatcherQueue dispatcher)
    {
        (_library, Playout, _dispatcher) = (library, playout, dispatcher);
        _showId = library.Document.ShowId;
        _library.Changed += OnLibraryChanged;
        _timer = dispatcher.CreateTimer();
        _timer.Interval = TimeSpan.FromSeconds(1);
        _timer.Tick += OnTick;
        _timer.Start();
    }

    internal GraphicsSocialPlayout Playout { get; }
    internal string VideoId { get; private set; } = "";
    internal string Status { get; private set; } = "Disconnected. Connect a public YouTube livestream.";
    internal bool Running => _connection is not null;
    internal event EventHandler? Changed;

    internal async Task ConnectAsync(string videoId, string key)
    {
        if (!YouTubeSocialFeed.IsVideoId(videoId))
        {
            SetStatus("Enter the 11-character video ID of a public YouTube livestream.");
            return;
        }
        await _library.LoadAsync();
        await DisconnectAsync();
        var showId = _showId;
        try
        {
            var resolved = await Task.Run(() =>
            {
                if (key.Length == 0) return _credentials.Read();
                _credentials.Store(key);
                return key;
            });
            if (_closed || _showId != showId) return;
            if (resolved is null)
            {
                SetStatus("Enter a YouTube Data API key.");
                return;
            }
            VideoId = videoId;
            var connection = new CancellationTokenSource();
            _connection = connection;
            var generation = ++_generation;
            SetStatus("Connecting to YouTube…");
            _feed = RunFeedAsync(videoId, resolved, connection, generation);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or CryptographicException or InvalidOperationException)
        {
            SetStatus("The local YouTube key could not be saved. Check the key and local access.");
        }
    }

    internal async Task DisconnectAsync()
    {
        ++_generation;
        var connection = _connection;
        _connection = null;
        connection?.Cancel();
        await Playout.DisconnectAsync();
        await _feed;
        SetStatus("Disconnected. Review decisions cleared.");
    }

    internal async Task ClearAsync()
    {
        await DisconnectAsync();
        await Task.Run(_credentials.Clear);
        VideoId = "";
        SetStatus("Local YouTube key and review data cleared.");
    }

    internal void Decide(GraphicsSocialItemKey key, bool approve)
    {
        var decided = approve ? Playout.Approve(key) : Playout.Reject(key);
        SetStatus(decided ? (approve ? "Approved. Use Show approved to put it on air." : "Rejected.") :
            "That version has changed. Select the current message and review it.");
    }

    internal async Task ShowAsync(GraphicsSocialItemKey key)
    {
        var result = await Playout.ShowAsync(key);
        SetStatus(result.Accepted ? "Show requested. Program confirms when the graphic is on air." : result.Reason);
    }

    internal async Task HideAsync()
    {
        SetStatus(await Playout.HideAsync() ? "Social graphic hidden." : "Could not clear the social graphic. It will expire.");
    }

    internal async Task WithdrawAsync(GraphicsSocialItemKey key)
    {
        SetStatus(await Playout.WithdrawAsync(key) ? "Message withdrawn." :
            "That version has changed. Select the current message.");
    }

    internal async Task StopForShutdownAsync()
    {
        if (_closed) return;
        _closed = true;
        _library.Changed -= OnLibraryChanged;
        _timer.Stop();
        _timer.Tick -= OnTick;
        try { await DisconnectAsync(); }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException) { }
    }

    private async Task RunFeedAsync(string videoId, string key, CancellationTokenSource connection, int generation)
    {
        try
        {
            await YouTubeSocialFeed.RunAsync(videoId, key, Playout, status =>
                _dispatcher.TryEnqueue(() => { if (_generation == generation) SetStatus($"{videoId} · {status}"); }), connection.Token);
        }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException)
        {
            if (_generation == generation) SetStatus("YouTube review stopped. Reconnect to try again.");
        }
        finally
        {
            if (ReferenceEquals(_connection, connection)) _connection = null;
            connection.Dispose();
            if (!_closed) Changed?.Invoke(this, EventArgs.Empty);
        }
    }

    private async void OnLibraryChanged(object? sender, EventArgs args)
    {
        if (_closed || _library.Document.ShowId == _showId) return;
        _showId = _library.Document.ShowId;
        try { await DisconnectAsync(); }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException)
        { SetStatus("The show changed. Social review stopped."); }
    }

    private async void OnTick(DispatcherQueueTimer sender, object args)
    {
        if (_closed || _sweeping) return;
        _sweeping = true;
        try { await Playout.ExpireAsync(); }
        catch (Exception error) when (error is IOException or InvalidOperationException or OperationCanceledException)
        { SetStatus("Could not refresh social review."); }
        finally { _sweeping = false; }
        if (!_closed) Changed?.Invoke(this, EventArgs.Empty);
    }

    private void SetStatus(string status)
    {
        Status = status;
        if (!_closed) Changed?.Invoke(this, EventArgs.Empty);
    }
}

public sealed partial class StudioViewModel
{
    private GraphicsSocialReviewViewModel? _graphicsSocialReview;
    private Task _graphicsSocialShutdown = Task.CompletedTask;
    internal GraphicsSocialReviewViewModel GraphicsSocialReview =>
        _graphicsSocialReview ??= new(GraphicsLibrary,
            (_graphicsCommands ??= new GraphicsCommandGateway(_bridge)).CreateSocialPlayout(), _dispatcher);
}
