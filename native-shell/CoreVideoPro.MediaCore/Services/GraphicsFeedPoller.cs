using CoreVideoPro.MediaCore.Models;

namespace CoreVideoPro.MediaCore.Services;

public sealed record GraphicsFeedDefinition(string Id, string Name, string Format, string Endpoint, string KeyColumn,
    bool Enabled = false, int PollSeconds = 15, long Generation = 0)
{
    public bool Valid() => Id is not null && GraphicsPlaylistRow.ValidId(Id) &&
        Name is { Length: > 0 and <= 128 } && !string.IsNullOrWhiteSpace(Name) &&
        Format is "sheets" or "rss" or "json" or "wss" && KeyColumn is { Length: > 0 and <= 128 } &&
        (Format != "rss" || KeyColumn == "id") && PollSeconds is >= 5 and <= 300 && Generation >= 0 &&
        Endpoint is { Length: > 0 and <= 2048 } && Uri.TryCreate(Endpoint, UriKind.Absolute, out var uri) &&
        uri.Scheme == (Format == "wss" ? "wss" : Uri.UriSchemeHttps) && uri.Port == 443 && uri.UserInfo.Length == 0 &&
        uri.Fragment.Length == 0 && uri.Host.Length > 0;
}

public enum GraphicsFeedStage { Updated, Unchanged, Rejected, WatermarkAdvanced, StaleSequence }

public sealed record GraphicsFeedStatus(string State = "disabled", DateTimeOffset? LastSuccess = null,
    DateTimeOffset? NextCheck = null, string? Failure = null);

/// <summary>One poller per source, driven by its owning dispatcher. The commit
/// delegate must also fence writes against the saved source generation.</summary>
public sealed class GraphicsFeedPoller(
    Func<GraphicsFeedDefinition, CancellationToken, Task<GraphicsLocalData>> fetch,
    Func<GraphicsFeedDefinition, GraphicsLocalData, CancellationToken, Task<GraphicsFeedStage>> stage,
    TimeProvider clock) : IDisposable
{
    private GraphicsFeedDefinition? _definition;
    private GraphicsFeedStatus _status = new();
    private CancellationTokenSource? _inFlight;
    private DateTimeOffset? _started;
    private int _failures;
    private bool _disposed;

    public GraphicsFeedStatus Status => _definition is { Enabled: true } feed &&
        _status.LastSuccess is { } success && clock.GetUtcNow() - success >= TimeSpan.FromSeconds(feed.PollSeconds * 2)
        ? _status with { State = "stale" } : _status;

    public void Configure(GraphicsFeedDefinition definition)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!definition.Valid()) throw new ArgumentException("Invalid graphics feed definition.", nameof(definition));
        if (_definition == definition) return;
        var lastSuccess = _definition is { } previous && previous.Endpoint == definition.Endpoint &&
            previous.Format == definition.Format && previous.KeyColumn == definition.KeyColumn ? _status.LastSuccess : null;
        _definition = definition;
        _failures = 0;
        _status = definition.Enabled ? new("waiting", lastSuccess, EarliestCheck()) : new(LastSuccess: lastSuccess);
        _inFlight?.Cancel();
    }

    public void RefreshNow()
    {
        if (_definition is { Enabled: true } && !_disposed)
            _status = _status with { NextCheck = EarliestCheck() };
    }

    private DateTimeOffset EarliestCheck()
    {
        var now = clock.GetUtcNow();
        return _started?.AddSeconds(5) is { } floor && floor > now ? floor : now;
    }

    public async Task TickAsync()
    {
        if (_disposed || _definition is not { Enabled: true } feed || _inFlight is not null ||
            _status.NextCheck is not { } due || clock.GetUtcNow() < due) return;
        using var operation = new CancellationTokenSource();
        _inFlight = operation;
        _started = clock.GetUtcNow();
        _status = _status with { State = "refreshing" };
        string? failure = null;
        try
        {
            GraphicsLocalData data;
            using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10), clock))
            using (var request = CancellationTokenSource.CreateLinkedTokenSource(operation.Token, timeout.Token))
            {
                data = await fetch(feed, request.Token);
                request.Token.ThrowIfCancellationRequested();
            }
            operation.Token.ThrowIfCancellationRequested();
            if (await stage(feed, data, operation.Token) == GraphicsFeedStage.Rejected)
                failure = "rejected";
        }
        catch (OperationCanceledException) when (operation.IsCancellationRequested) { return; }
        catch (OperationCanceledException) { failure = "timeout"; }
        catch (InvalidDataException) { failure = "invalid-data"; }
        catch (HttpRequestException) { failure = "unavailable"; }
        catch (IOException) { failure = "unavailable"; }
        finally { _inFlight = null; }

        if (operation.IsCancellationRequested) return;
        var now = clock.GetUtcNow();
        if (failure is null)
        {
            _failures = 0;
            _status = new("fresh", now, now.AddSeconds(feed.PollSeconds));
        }
        else
        {
            _failures = Math.Min(6, _failures + 1);
            var retry = Math.Min(300, feed.PollSeconds * (1 << _failures));
            _status = new("failed", _status.LastSuccess, now.AddSeconds(retry), failure);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _inFlight?.Cancel();
    }
}
