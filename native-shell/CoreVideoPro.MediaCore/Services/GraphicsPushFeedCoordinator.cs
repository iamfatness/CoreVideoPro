namespace CoreVideoPro.MediaCore.Services;

/// <summary>Driven by the show dispatcher. Each background reader owns one replaceable snapshot slot.</summary>
public sealed class GraphicsPushFeedCoordinator : IDisposable
{
    private static readonly TimeSpan RetryFloor = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan StageCadence = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan StaleAfter = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan ReconnectAfter = TimeSpan.FromSeconds(60);
    private readonly Func<GraphicsFeedDefinition, GraphicsPushReceiveBudget, Action, Action<byte[]>, CancellationToken, Task> _receive;
    private readonly Func<GraphicsFeedDefinition, GraphicsFeedSnapshot, DateTimeOffset, CancellationToken, Task<GraphicsFeedStage>> _stage;
    private readonly TimeProvider _clock;
    private readonly Func<Func<Task>, Task> _run;
    private readonly GraphicsPushReceiveBudget _budget;
    private GraphicsFeedDefinition? _definition;
    private GraphicsDataColumn[]? _columns;
    private GraphicsFeedStatus _status = new();
    private Attempt? _attempt;
    private Write? _write;
    private long? _lastConnect;
    private long? _lastWrite;
    private long? _lastAccepted;
    private DateTimeOffset? _savedReceivedAt;
    private long _retrySince;
    private TimeSpan _retryDelay;
    private int _failures;
    private bool _manual;
    private bool _disposed;

    public GraphicsPushFeedCoordinator(
        Func<GraphicsFeedDefinition, GraphicsFeedSnapshot, DateTimeOffset, CancellationToken, Task<GraphicsFeedStage>> stage,
        TimeProvider clock) : this(ReceiveAsync, stage, clock, work => Task.Run(work)) { }

    internal GraphicsPushFeedCoordinator(
        Func<GraphicsFeedDefinition, GraphicsPushReceiveBudget, Action, Action<byte[]>, CancellationToken, Task> receive,
        Func<GraphicsFeedDefinition, GraphicsFeedSnapshot, DateTimeOffset, CancellationToken, Task<GraphicsFeedStage>> stage,
        TimeProvider clock, Func<Func<Task>, Task> run)
    {
        (_receive, _stage, _clock, _run) = (receive, stage, clock, run);
        _budget = new(clock);
    }

    public GraphicsFeedStatus Status
    {
        get
        {
            if (_definition is not { Enabled: true } || _disposed) return _status;
            var wait = _manual ? Remaining(_lastConnect, RetryFloor) :
                _attempt is null ? Remaining(_retrySince, _retryDelay) : (TimeSpan?)null;
            var stale = _lastAccepted is { } accepted ? _clock.GetElapsedTime(accepted) >= StaleAfter :
                _savedReceivedAt is { } received && _clock.GetUtcNow() - received >= StaleAfter;
            return _status with
            {
                State = stale ? "stale" : _status.State,
                NextCheck = wait is { } delay ? _clock.GetUtcNow() + delay : null
            };
        }
    }

    public void Configure(GraphicsFeedDefinition definition, GraphicsDataSource? saved = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!definition.Valid() || definition.Format != "wss")
            throw new ArgumentException("Invalid graphics push feed definition.", nameof(definition));

        _columns = saved?.Data.Columns;
        _savedReceivedAt = saved?.ReceivedAt;
        if (_definition == definition)
        {
            if (saved?.FeedSequence is { } accepted) _status = _status with { AcceptedSequence = accepted };
            _attempt?.SetColumns(_columns);
            return;
        }

        StopAttempt();
        var sameSource = _definition is { } old && old.Id == definition.Id &&
            old.Endpoint == definition.Endpoint && old.KeyColumn == definition.KeyColumn;
        if (!sameSource) _lastAccepted = null;
        _definition = definition;
        _status = new(definition.Enabled ? "waiting" : "disabled", sameSource ? _status.LastSuccess : null)
        {
            AcceptedSequence = saved?.FeedSequence
        };
        _failures = 0;
        _manual = false;
        _retrySince = _clock.GetTimestamp();
        _retryDelay = Remaining(_lastConnect, RetryFloor);
    }

    public void ReconnectNow()
    {
        if (!_disposed && _definition is { Enabled: true }) _manual = true;
    }

    public void Tick()
    {
        if (_disposed) return;
        FinishWrite();
        if (_definition is not { Enabled: true } feed) return;

        if (_manual && Remaining(_lastConnect, RetryFloor) == TimeSpan.Zero)
        {
            StopAttempt();
            StartAttempt(feed);
        }
        if (_attempt is null)
        {
            if (!_manual && Remaining(_retrySince, _retryDelay) == TimeSpan.Zero) StartAttempt(feed);
            return;
        }

        var attempt = _attempt;
        var progress = attempt.Progress();
        if (progress.ReadCount != attempt.ObservedReads)
        {
            attempt.ObservedReads = progress.ReadCount;
            _status = _status with { LastRead = progress.LastRead, LastValidated = progress.LastValidated, Failure = progress.Rejection };
        }
        if (progress.ConnectedAt is not null && _status.State == "connecting")
            _status = _status with { State = "connected" };

        var since = attempt.LastAccepted ?? progress.ConnectedAt;
        if (since is { } connected && _clock.GetElapsedTime(connected) >= ReconnectAfter)
        {
            Retry("idle");
            return;
        }

        if (_write is null && Remaining(_lastWrite, StageCadence) == TimeSpan.Zero &&
            attempt.TakeWaiting() is { } waiting)
        {
            _lastWrite = _clock.GetTimestamp();
            _write = new(attempt, waiting, StageAsync(feed, waiting, attempt.Token));
            FinishWrite();
        }

        // Let a completed connection's last snapshot finish staging before retrying.
        if (progress.End is { } failure && _write?.Attempt != attempt && !attempt.HasWaiting)
            Retry(failure);
    }

    private void StartAttempt(GraphicsFeedDefinition feed)
    {
        _manual = false;
        _lastConnect = _clock.GetTimestamp();
        _status = _status with { State = "connecting", Failure = null, LastRead = null, LastValidated = null };
        var attempt = new Attempt(_clock, feed.KeyColumn, _columns);
        _attempt = attempt;
        attempt.Reader = _run(() => PumpAsync(feed, attempt));
    }

    private async Task PumpAsync(GraphicsFeedDefinition feed, Attempt attempt)
    {
        try
        {
            await _receive(feed, _budget, attempt.Connected, attempt.Deliver, attempt.Token).ConfigureAwait(false);
            attempt.End("unavailable");
        }
        catch (OperationCanceledException) when (attempt.Token.IsCancellationRequested) { }
        catch (TimeoutException) { attempt.End("timeout"); }
        catch (InvalidDataException) { attempt.End("invalid-data"); }
        catch (Exception ex) when (ex is not OutOfMemoryException) { attempt.End("unavailable"); }
    }

    private static async Task ReceiveAsync(GraphicsFeedDefinition feed, GraphicsPushReceiveBudget budget,
        Action connected, Action<byte[]> deliver, CancellationToken cancellation)
    {
        using var socket = await new GraphicsPushTransport().ConnectAsync(feed, budget, cancellation).ConfigureAwait(false);
        connected();
        while (await socket.ReadAsync(cancellation).ConfigureAwait(false) is { } bytes)
            deliver(bytes);
    }

    private async Task<GraphicsFeedStage> StageAsync(GraphicsFeedDefinition feed, Snapshot waiting, CancellationToken cancellation)
    {
        try { return await _stage(feed, waiting.Value, waiting.ReceivedAt, cancellation); }
        catch (Exception ex) when (ex is not OutOfMemoryException) { return GraphicsFeedStage.Rejected; }
    }

    private void FinishWrite()
    {
        if (_write is not { Result.IsCompletedSuccessfully: true } write) return;
        _write = null;
        if (write.Attempt != _attempt || write.Attempt.Token.IsCancellationRequested) return;
        switch (write.Result.Result)
        {
            case GraphicsFeedStage.Updated:
            case GraphicsFeedStage.Unchanged:
            case GraphicsFeedStage.WatermarkAdvanced:
                _failures = 0;
                _lastAccepted = write.Attempt.LastAccepted = _clock.GetTimestamp();
                _status = _status with
                {
                    State = "fresh", LastSuccess = _clock.GetUtcNow(), AcceptedSequence = write.Snapshot.Value.Sequence,
                    Failure = write.Attempt.Progress().Rejection
                };
                break;
            case GraphicsFeedStage.Rejected:
                _status = _status with { Failure = "rejected" };
                break;
        }
    }

    private TimeSpan Remaining(long? since, TimeSpan interval)
    {
        if (since is null) return TimeSpan.Zero;
        var remaining = interval - _clock.GetElapsedTime(since.Value);
        return remaining > TimeSpan.Zero ? remaining : TimeSpan.Zero;
    }

    private void Retry(string failure)
    {
        StopAttempt();
        _retrySince = _clock.GetTimestamp();
        _retryDelay = TimeSpan.FromSeconds(Math.Min(300, 5 << _failures));
        _failures = Math.Min(6, _failures + 1);
        _status = _status with { State = "retrying", Failure = failure };
    }

    private void StopAttempt()
    {
        if (_attempt is not { } attempt) return;
        _attempt = null;
        _ = attempt.StopAsync(_write?.Attempt == attempt ? _write.Result : Task.CompletedTask);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        StopAttempt();
        _write = null;
        _status = new();
    }

    private sealed record Snapshot(GraphicsFeedSnapshot Value, DateTimeOffset ReceivedAt);
    private sealed record Write(Attempt Attempt, Snapshot Snapshot, Task<GraphicsFeedStage> Result);
    private sealed record ReadProgress(long ReadCount = 0, DateTimeOffset? LastRead = null, DateTimeOffset? LastValidated = null,
        long? ConnectedAt = null, string? Rejection = null, string? End = null);

    private sealed class Attempt(TimeProvider clock, string keyColumn, GraphicsDataColumn[]? columns)
    {
        private readonly CancellationTokenSource _lifetime = new();
        private readonly object _gate = new();
        private ReadProgress _progress = new();
        private Snapshot? _waiting;

        public Task Reader { get; set; } = Task.CompletedTask;
        public long ObservedReads { get; set; }
        public long? LastAccepted { get; set; }
        public CancellationToken Token => _lifetime.Token;

        public ReadProgress Progress() { lock (_gate) return _progress; }
        public bool HasWaiting { get { lock (_gate) return _waiting is not null; } }
        public void Connected() { lock (_gate) _progress = _progress with { ConnectedAt = clock.GetTimestamp() }; }
        public void End(string category) { lock (_gate) _progress = _progress with { End = category }; }

        public void SetColumns(GraphicsDataColumn[]? current)
        {
            lock (_gate)
            {
                columns = current;
                if (_waiting is { } waiting && !Compatible(waiting.Value)) _waiting = null;
            }
        }

        public void Deliver(byte[] bytes)
        {
            if (_lifetime.IsCancellationRequested) return;
            var received = clock.GetUtcNow();
            GraphicsFeedSnapshot? snapshot = null;
            try { snapshot = GraphicsFeedSnapshot.Parse(bytes, keyColumn); }
            catch (InvalidDataException) { }

            lock (_gate)
            {
                if (_lifetime.IsCancellationRequested) return;
                var valid = snapshot is not null && Compatible(snapshot);
                var conflicting = valid && _waiting is { } queued && snapshot!.Sequence == queued.Value.Sequence &&
                    !queued.Value.Data.HasSameRows(snapshot.Data);
                _progress = _progress with
                {
                    ReadCount = _progress.ReadCount + 1, LastRead = received,
                    LastValidated = valid ? received : _progress.LastValidated,
                    Rejection = !valid ? "invalid-data" : conflicting ? "rejected" : null
                };
                if (valid && (_waiting is null || snapshot!.Sequence > _waiting.Value.Sequence))
                    _waiting = new(snapshot!, received);
            }
        }

        private bool Compatible(GraphicsFeedSnapshot snapshot) => columns is null || columns.SequenceEqual(snapshot.Data.Columns);

        public Snapshot? TakeWaiting()
        {
            lock (_gate)
            {
                var result = _waiting;
                _waiting = null;
                return result;
            }
        }

        public async Task StopAsync(Task write)
        {
            try
            {
                var canceled = _lifetime.CancelAsync();
                lock (_gate) _waiting = null;
                await Task.WhenAll(canceled, Reader, write).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is not OutOfMemoryException) { }
            finally { _lifetime.Dispose(); }
        }
    }
}
