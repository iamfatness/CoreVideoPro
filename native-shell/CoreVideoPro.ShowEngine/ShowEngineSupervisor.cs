using System.Collections.Concurrent;
using System.Text.Json;

namespace CoreVideoPro.ShowEngine;

public sealed class ShowEngineSupervisorOptions
{
    public TimeSpan RequestTimeout { get; init; } = TimeSpan.FromSeconds(4);
    public TimeSpan HandshakeTimeout { get; init; } = TimeSpan.FromSeconds(15);
    public TimeSpan HeartbeatInterval { get; init; } = TimeSpan.FromSeconds(1);
    public int MissedHeartbeatsBeforeHang { get; init; } = 2;

    /// <summary>How long <see cref="ShowEngineSupervisor.StopAsync"/> waits for a clean exit after
    /// <c>shutdown</c> before killing the tree.</summary>
    public TimeSpan StopGrace { get; init; } = TimeSpan.FromSeconds(1.5);
}

/// <summary>
/// Spawn / read / write / recover for the OHG show engine child. Shaped after
/// <c>MediaCoreSupervisor</c> — a stdout loop with id correlation and a reference-equality generation
/// guard, a <see cref="SemaphoreSlim"/> stdin gate whose timeout covers write+flush+response, and a
/// kill-tree teardown — but only for this one much smaller protocol.
///
/// <para><b>Timing contract (this is what makes the class testable).</b> The supervisor NEVER calls
/// <c>Thread.Sleep</c>, <c>Task.Delay</c>, or a timer. Every wait — the request timeout, the handshake
/// fallback timeout, the heartbeat interval, the restart backoff, the stop grace — goes through the
/// injected <c>delay(TimeSpan, CancellationToken)</c>, and every timestamp through the injected
/// <c>now()</c>. In particular the request timeout is <c>delay(RequestTimeout, ct)</c> RACED against
/// the response: a test whose <c>delay</c> completes immediately for that duration sees a
/// <see cref="TimeoutException"/> deterministically; a test whose <c>delay</c> parks forever sees no
/// timeout, ever. Production passes <c>(d, ct) =&gt; Task.Delay(d, ct)</c>.</para>
///
/// <para><b>Threading.</b> <see cref="HealthChanged"/>, <see cref="Handshaken"/>,
/// <see cref="SnapshotReceived"/>, <see cref="HostCommandReceived"/> and <see cref="LogReceived"/> may
/// all be raised on the reader thread (a thread-pool thread), never the UI thread. CONSUMERS MUST
/// MARSHAL — in the WinUI shell that means <c>UiDispatch</c>, per this repo's rule that a throwing
/// queued callback fail-fasts the process.</para>
/// </summary>
public sealed class ShowEngineSupervisor : IDisposable
{
    private sealed record Pending(IShowEngineChild Child, TaskCompletionSource<JsonDocument> Completion);

    private readonly IShowEngineChildFactory _factory;
    private readonly ShowEngineRestartPolicy _policy;
    private readonly Func<TimeSpan, CancellationToken, Task> _delay;
    private readonly Func<DateTimeOffset> _now;
    private readonly ShowEngineSupervisorOptions _options;

    private readonly ConcurrentDictionary<string, Pending> _pending = new(StringComparer.Ordinal);
    private readonly SemaphoreSlim _stdinGate = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private readonly object _gate = new();

    private IShowEngineChild? _child;
    private ShowEngineSpawnRequest? _request;
    private CancellationTokenSource? _childScope;
    private TaskCompletionSource<JsonElement>? _handshakeSignal;
    private int _spawnGeneration;
    private int _currentGeneration;
    private int _requestCounter;
    private long _staleHostCommandsDropped;
    private bool _stopping;
    private bool _disposed;
    private ShowEngineHealth _health = new(ShowEngineState.Stopped, 0, 0, null, null);

    /// <param name="delay">The ONLY source of elapsed time in this class — see the timing contract on
    /// the type. Production: <c>(d, ct) =&gt; Task.Delay(d, ct)</c>.</param>
    /// <param name="now">The ONLY clock — used for <c>LastCrashAt</c> and every policy callback.</param>
    public ShowEngineSupervisor(
        IShowEngineChildFactory factory,
        ShowEngineRestartPolicy policy,
        Func<TimeSpan, CancellationToken, Task> delay,
        Func<DateTimeOffset> now,
        ShowEngineSupervisorOptions? options = null)
    {
        _factory = factory;
        _policy = policy;
        _delay = delay;
        _now = now;
        _options = options ?? new ShowEngineSupervisorOptions();
    }

    public ShowEngineHealth Health { get { lock (_gate) return _health; } }

    public event Action<ShowEngineHealth>? HealthChanged;
    public event Action<ShowEngineHandshake>? Handshaken;
    public event Action<ShowEngineSnapshot>? SnapshotReceived;
    public event Action<ShowEngineHostCommand>? HostCommandReceived;
    public event Action<ShowEngineLogLine>? LogReceived;

    /// <summary>How many <c>hostCommand</c> events arrived stamped with a generation that is no longer
    /// current and were therefore dropped. A rising count on a healthy show means the engine is
    /// echoing a stale generation — worth surfacing, never worth acting on.</summary>
    public long StaleHostCommandsDropped => Interlocked.Read(ref _staleHostCommandsDropped);

    // ------------------------------------------------------------------ lifecycle

    /// <summary>Spawn and handshake. Returns once the engine is <see cref="ShowEngineState.Running"/>
    /// or <see cref="ShowEngineState.Failed"/> — it does not throw on a failed start; read
    /// <see cref="Health"/>.</summary>
    public async Task StartAsync(ShowEngineSpawnRequest request, CancellationToken ct)
    {
        lock (_gate)
        {
            _request = request;
            _stopping = false;
        }

        SetHealth(h => h with { State = ShowEngineState.Starting, LastError = null });
        await SpawnAndHandshakeAsync(request, ct).ConfigureAwait(false);
    }

    /// <summary>Operator-initiated restart: tear the child down, forget the failure history, start
    /// again. Distinct from a crash recovery, which must NOT reset the policy.</summary>
    public async Task RestartAsync(ShowEngineSpawnRequest request, CancellationToken ct)
    {
        await StopAsync().ConfigureAwait(false);
        _policy.Reset();
        await StartAsync(request, ct).ConfigureAwait(false);
    }

    /// <summary>Ask the engine to exit, give it <see cref="ShowEngineSupervisorOptions.StopGrace"/>,
    /// then kill the tree. Never recovers afterwards; <see cref="Health"/> ends
    /// <see cref="ShowEngineState.Stopped"/>.</summary>
    public async Task StopAsync()
    {
        IShowEngineChild? child;
        lock (_gate)
        {
            _stopping = true;
            child = _child;
        }

        if (child is null)
        {
            SetHealth(h => h with { State = ShowEngineState.Stopped });
            return;
        }

        try
        {
            await SendCoreAsync(child, "shutdown", null, _options.RequestTimeout, CancellationToken.None)
                .ConfigureAwait(false);
        }
        catch
        {
            // A dying engine legitimately fails to answer its own shutdown; the grace + kill below is
            // the real contract.
        }

        var grace = _delay(_options.StopGrace, CancellationToken.None);
        if (await Task.WhenAny(child.Exited, grace).ConfigureAwait(false) != (Task)child.Exited &&
            !child.Exited.IsCompleted)
        {
            child.Kill();
        }

        TeardownChild(child, kill: false);
        RejectPendingFor(child, new InvalidOperationException("Show engine stopped."));
        SetHealth(h => h with { State = ShowEngineState.Stopped });
    }

    public void Dispose()
    {
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            _stopping = true;
        }

        try { _lifetime.Cancel(); } catch { /* best effort */ }
        var child = _child;
        if (child is not null) TeardownChild(child, kill: true);
        RejectAll(new ObjectDisposedException(nameof(ShowEngineSupervisor)));
        _lifetime.Dispose();
        _stdinGate.Dispose();
    }

    // ------------------------------------------------------------------ requests

    /// <summary>Send one request and await its correlated response. Throws
    /// <see cref="InvalidOperationException"/> when the engine is not Running, and
    /// <see cref="TimeoutException"/> when <c>RequestTimeout</c> elapses — the timeout covers the
    /// stdin gate, the write, the flush AND the response, the way <c>MediaCoreSupervisor</c> learned
    /// it must (a child that stalls reading stdin fills the pipe buffer and the WRITE is what hangs).
    /// The returned document is the caller's to dispose.</summary>
    public Task<JsonDocument> SendAsync(string type, object? payload, CancellationToken ct)
    {
        IShowEngineChild child;
        lock (_gate)
        {
            if (_health.State != ShowEngineState.Running || _child is null)
            {
                throw new InvalidOperationException(
                    $"OHG show engine is {_health.State.ToString().ToLowerInvariant()}.");
            }

            child = _child;
        }

        return SendCoreAsync(child, type, payload, _options.RequestTimeout, ct);
    }

    private async Task<JsonDocument> SendCoreAsync(
        IShowEngineChild child, string type, object? payload, TimeSpan timeout, CancellationToken ct)
    {
        var id = "se-" + Interlocked.Increment(ref _requestCounter).ToString();
        var completion = new TaskCompletionSource<JsonDocument>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = new Pending(child, completion);

        using var scope = CancellationTokenSource.CreateLinkedTokenSource(ct, _lifetime.Token);
        try
        {
            var work = WriteThenAwaitAsync(child, ShowEngineProtocol.EncodeRequest(id, type, payload),
                completion.Task, scope.Token);
            var timeoutTask = _delay(timeout, scope.Token);

            if (await Task.WhenAny(work, timeoutTask).ConfigureAwait(false) != work && !work.IsCompleted)
            {
                ct.ThrowIfCancellationRequested();
                throw new TimeoutException($"show engine request {id} ({type}) timed out.");
            }

            return await work.ConfigureAwait(false);
        }
        finally
        {
            _pending.TryRemove(id, out _);
            // Releases a parked timeout delay so it cannot leak past this request.
            try { scope.Cancel(); } catch { /* best effort */ }
        }
    }

    private async Task<JsonDocument> WriteThenAwaitAsync(
        IShowEngineChild child, string line, Task<JsonDocument> response, CancellationToken ct)
    {
        await _stdinGate.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            await child.WriteLineAsync(line, ct).ConfigureAwait(false);
        }
        finally
        {
            _stdinGate.Release();
        }

        return await response.ConfigureAwait(false);
    }

    // ------------------------------------------------------------------ spawn + handshake

    private async Task<bool> SpawnAndHandshakeAsync(ShowEngineSpawnRequest request, CancellationToken ct)
    {
        var generation = Interlocked.Increment(ref _spawnGeneration);
        var handshakeSignal = new TaskCompletionSource<JsonElement>(TaskCreationOptions.RunContinuationsAsynchronously);
        var scope = CancellationTokenSource.CreateLinkedTokenSource(ct, _lifetime.Token);

        IShowEngineChild child;
        try
        {
            child = _factory.Spawn(generation, request);
        }
        catch (Exception ex)
        {
            scope.Dispose();
            SetHealth(h => h with { State = ShowEngineState.Failed, LastError = ex.Message });
            return false;
        }

        lock (_gate)
        {
            _child = child;
            _childScope = scope;
            _handshakeSignal = handshakeSignal;
        }

        _ = Task.Run(() => ReaderLoopAsync(child, scope), CancellationToken.None);
        _ = Task.Run(() => WatchExitAsync(child), CancellationToken.None);

        // The host writes an unsolicited handshake EVENT at startup. Only when that does not arrive in
        // time do we ask for one explicitly (which the host answers with a response AND another event).
        // child.Exited is in the race so a child that dies before announcing does not hold the start
        // for the full HandshakeTimeout — the exit watcher has already begun recovery by then.
        var wait = _delay(_options.HandshakeTimeout, scope.Token);
        await Task.WhenAny(handshakeSignal.Task, wait, child.Exited).ConfigureAwait(false);
        if (!handshakeSignal.Task.IsCompleted && !child.Exited.IsCompleted)
        {
            try
            {
                // HandshakeTimeout governs how long we wait for the UNSOLICITED announcement. Once we
                // are actively asking, the ordinary request timeout applies — a host that answers no
                // request at all is dead, and waiting another 15 s only delays the recovery.
                using var reply = await SendCoreAsync(child, "handshake", null, _options.RequestTimeout, scope.Token)
                    .ConfigureAwait(false);
                // Some hosts (and Task 6's fixtures) carry the manifest on the RESPONSE itself; the real
                // host follows it with the event, which the reader loop signals just as well.
                if (reply.RootElement.TryGetProperty("protocolVersion", out _))
                {
                    handshakeSignal.TrySetResult(reply.RootElement.Clone());
                }
                else
                {
                    var second = _delay(_options.HandshakeTimeout, scope.Token);
                    await Task.WhenAny(handshakeSignal.Task, second).ConfigureAwait(false);
                }
            }
            catch (Exception ex)
            {
                FailStart(child, $"show engine handshake failed: {ex.Message}");
                return false;
            }
        }

        // A crash during the handshake wait already handed this generation to the recovery path, which
        // may have spawned its successor. Anything below would publish health for a child that is no
        // longer ours, so a superseded start simply withdraws.
        lock (_gate)
        {
            if (!ReferenceEquals(_child, child)) return false;
        }

        if (!handshakeSignal.Task.IsCompletedSuccessfully)
        {
            FailStart(child, "show engine did not handshake");
            return false;
        }

        var root = handshakeSignal.Task.Result;
        if (!ShowEngineProtocol.TryParseHandshake(root, out var handshake, out var error))
        {
            FailStart(child, error ?? "unreadable show engine handshake");
            return false;
        }

        if (handshake.ProtocolVersion != ShowEngineProtocol.SupportedProtocolVersion)
        {
            // NOT a transient fault: a newer engine will never become compatible by restarting, so this
            // is Failed with no respawn, exactly like a config error.
            FailStart(child, $"unsupported protocol version {handshake.ProtocolVersion}");
            return false;
        }

        Volatile.Write(ref _currentGeneration, handshake.Generation);
        _policy.RecordRunning(_now());
        SetHealth(h => h with
        {
            State = ShowEngineState.Running,
            Generation = handshake.Generation,
            LastError = null
        });

        Raise(Handshaken, handshake);
        _ = Task.Run(() => HeartbeatLoopAsync(child, scope.Token), CancellationToken.None);
        return true;
    }

    private void FailStart(IShowEngineChild child, string error)
    {
        TeardownChild(child, kill: true);
        RejectPendingFor(child, new InvalidOperationException(error));
        SetHealth(h => h with { State = ShowEngineState.Failed, LastError = error });
    }

    // ------------------------------------------------------------------ reader loop

    /// <summary>Drains one child's stdout until EOF or teardown, then OWNS disposing that child and
    /// its cancellation scope. Nothing else disposes them, which is what lets a child that exited
    /// naturally finish delivering the lines already in its pipe — including the log line that
    /// explains why it died — instead of having them thrown away with the process handle.</summary>
    private async Task ReaderLoopAsync(IShowEngineChild child, CancellationTokenSource scope)
    {
        var ct = scope.Token;
        try
        {
            while (!ct.IsCancellationRequested)
            {
                var line = await child.ReadLineAsync(ct).ConfigureAwait(false);
                if (line is null) break;           // EOF: the child's stdout closed
                if (line.Length == 0) continue;
                HandleLine(child, line);
            }
        }
        catch (OperationCanceledException)
        {
            // Teardown.
        }
        catch (Exception ex)
        {
            Raise(LogReceived, new ShowEngineLogLine("error", $"show engine reader failed: {ex.Message}"));
        }
        finally
        {
            try { child.Dispose(); } catch { /* best effort */ }
            try { scope.Dispose(); } catch { /* best effort */ }
        }
    }

    private void HandleLine(IShowEngineChild child, string line)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(line);
        }
        catch (JsonException ex)
        {
            Raise(LogReceived, new ShowEngineLogLine("warn", $"malformed show engine line: {ex.Message}"));
            return;
        }

        var dispose = true;
        try
        {
            switch (ShowEngineProtocol.Classify(doc))
            {
                case ShowEngineProtocol.LineKind.Response:
                    dispose = !CompleteResponse(child, doc);
                    break;

                case ShowEngineProtocol.LineKind.Handshake:
                    lock (_gate) { _handshakeSignal?.TrySetResult(doc.RootElement.Clone()); }
                    break;

                case ShowEngineProtocol.LineKind.Snapshot:
                    Raise(SnapshotReceived, ShowEngineProtocol.ParseSnapshot(doc.RootElement));
                    break;

                case ShowEngineProtocol.LineKind.HostCommand:
                    var command = ShowEngineProtocol.ParseHostCommand(doc.RootElement);
                    // A host command from a generation that is no longer current would act on a show
                    // state the engine that sent it no longer describes. Drop it, and COUNT it.
                    if (command.Generation != Volatile.Read(ref _currentGeneration))
                    {
                        Interlocked.Increment(ref _staleHostCommandsDropped);
                        break;
                    }

                    Raise(HostCommandReceived, command);
                    break;

                case ShowEngineProtocol.LineKind.Log:
                    Raise(LogReceived, ShowEngineProtocol.ParseLog(doc.RootElement));
                    break;

                case ShowEngineProtocol.LineKind.Malformed:
                    Raise(LogReceived, new ShowEngineLogLine("warn", "malformed show engine line: not an object"));
                    break;

                case ShowEngineProtocol.LineKind.Unknown:
                default:
                    break;   // a newer engine's event: ignore quietly, never warn per line
            }
        }
        finally
        {
            if (dispose) doc.Dispose();
        }
    }

    /// <summary>Complete the pending request this response correlates to. Returns true when ownership
    /// of <paramref name="doc"/> transferred to the awaiting caller.</summary>
    private bool CompleteResponse(IShowEngineChild child, JsonDocument doc)
    {
        if (!doc.RootElement.TryGetProperty("id", out var idElement) ||
            idElement.ValueKind != JsonValueKind.String ||
            idElement.GetString() is not { } id ||
            !_pending.TryGetValue(id, out var pending))
        {
            return false;
        }

        // THE GENERATION GUARD. Request ids restart at se-1 with every supervisor, and a dying child's
        // stdout can still be draining while its replacement is already taking requests — so a stale
        // child must never be allowed to answer a request that was issued to the CURRENT one.
        if (!ReferenceEquals(pending.Child, child)) return false;

        _pending.TryRemove(id, out _);

        if (doc.RootElement.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.False)
        {
            var message = doc.RootElement.TryGetProperty("error", out var error) &&
                          error.TryGetProperty("message", out var text)
                ? text.GetString() ?? "show engine request failed"
                : "show engine request failed";
            pending.Completion.TrySetException(new InvalidOperationException(message));
            return false;
        }

        return pending.Completion.TrySetResult(doc);
    }

    // ------------------------------------------------------------------ heartbeat + recovery

    private async Task HeartbeatLoopAsync(IShowEngineChild child, CancellationToken ct)
    {
        var missed = 0;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await _delay(_options.HeartbeatInterval, ct).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            lock (_gate)
            {
                if (ct.IsCancellationRequested || _stopping || !ReferenceEquals(_child, child)) return;
            }

            try
            {
                using var reply = await SendCoreAsync(child, "ping", null, _options.RequestTimeout, ct)
                    .ConfigureAwait(false);
                missed = 0;
                _policy.RecordHealthy(_now());
            }
            catch (TimeoutException)
            {
                if (++missed < _options.MissedHeartbeatsBeforeHang) continue;

                // A child that accepts stdin but never answers is WORSE than a crashed one: it holds the
                // show hostage with no exit code. Treat it as exit -1 and take the recovery path.
                Raise(LogReceived, new ShowEngineLogLine("warn",
                    $"show engine missed {missed} heartbeats — treating as a hang"));
                child.Kill();
                OnChildEnded(child, exitCode: -1);
                return;
            }
            catch
            {
                return;   // cancelled, disposed, or the child died — the exit watcher owns it
            }
        }
    }

    private async Task WatchExitAsync(IShowEngineChild child)
    {
        int exitCode;
        try
        {
            exitCode = await child.Exited.ConfigureAwait(false);
        }
        catch
        {
            exitCode = -1;
        }

        OnChildEnded(child, exitCode);
    }

    private void OnChildEnded(IShowEngineChild child, int exitCode)
    {
        ShowEngineSpawnRequest? request;
        lock (_gate)
        {
            if (_stopping || _disposed || !ReferenceEquals(_child, child)) return;
            request = _request;
        }

        // The process is already gone (or was just killed by the hang path), so this only DETACHES it.
        // Its reader loop is deliberately left running to drain whatever is still in the pipe; the
        // generation guard below is what keeps those late lines from touching the new child's state.
        DetachChild(child);
        RejectPendingFor(child, new InvalidOperationException("Show engine exited."));

        var crashedAt = _now();
        SetHealth(h => h with { RestartCount = h.RestartCount + 1, LastCrashAt = crashedAt });

        // Spec §9: exit 78 is the engine rejecting its CONFIG. Restarting cannot fix a bad config, so
        // there is no backoff and no respawn — just a loud, terminal state that names the log.
        if (exitCode == 78)
        {
            SetHealth(h => h with
            {
                State = ShowEngineState.Failed,
                LastError = "show engine rejected its config (exit 78) — see show-engine.log"
            });
            return;
        }

        SetHealth(h => h with
        {
            State = ShowEngineState.Recovering,
            LastError = $"show engine exited with code {exitCode}"
        });

        if (request is null)
        {
            SetHealth(h => h with { State = ShowEngineState.Failed, LastError = "no spawn request to recover with" });
            return;
        }

        _ = Task.Run(() => RecoverAsync(request), CancellationToken.None);
    }

    private async Task RecoverAsync(ShowEngineSpawnRequest request)
    {
        var delay = _policy.NextDelay(_now());
        if (delay is null)
        {
            SetHealth(h => h with
            {
                State = ShowEngineState.Failed,
                LastError = $"show engine failed {_policy.ConsecutiveFailures} times — press Restart"
            });
            return;
        }

        try
        {
            await _delay(delay.Value, _lifetime.Token).ConfigureAwait(false);
            await SpawnAndHandshakeAsync(request, _lifetime.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // Disposed mid-recovery.
        }
        catch (Exception ex)
        {
            SetHealth(h => h with { State = ShowEngineState.Failed, LastError = ex.Message });
        }
    }

    // ------------------------------------------------------------------ plumbing

    /// <summary>Stop treating <paramref name="child"/> as current. Returns its reader scope (null if it
    /// was already detached) WITHOUT cancelling it — the caller decides.</summary>
    private CancellationTokenSource? DetachChild(IShowEngineChild child)
    {
        lock (_gate)
        {
            if (!ReferenceEquals(_child, child)) return null;
            var scope = _childScope;
            _child = null;
            _childScope = null;
            return scope;
        }
    }

    /// <summary>Forced teardown: detach, optionally kill the tree, and end the reader loop (which then
    /// disposes the child and the scope).</summary>
    private void TeardownChild(IShowEngineChild child, bool kill)
    {
        var scope = DetachChild(child);
        if (kill)
        {
            try { child.Kill(); } catch { /* best effort */ }
        }

        try { scope?.Cancel(); } catch { /* best effort */ }
    }

    private void RejectPendingFor(IShowEngineChild child, Exception error)
    {
        foreach (var entry in _pending)
        {
            if (!ReferenceEquals(entry.Value.Child, child)) continue;
            if (_pending.TryRemove(entry.Key, out var pending)) pending.Completion.TrySetException(error);
        }
    }

    private void RejectAll(Exception error)
    {
        foreach (var entry in _pending)
        {
            if (_pending.TryRemove(entry.Key, out var pending)) pending.Completion.TrySetException(error);
        }
    }

    private void SetHealth(Func<ShowEngineHealth, ShowEngineHealth> mutate)
    {
        ShowEngineHealth updated;
        lock (_gate)
        {
            updated = mutate(_health);
            if (updated == _health) return;
            _health = updated;
        }

        Raise(HealthChanged, updated);
    }

    /// <summary>Raise an event without letting a subscriber's exception take the reader loop — or the
    /// process — with it (this repo's queued-callback fail-fast rule, applied at the source).</summary>
    private static void Raise<T>(Action<T>? handler, T payload)
    {
        if (handler is null) return;
        try
        {
            handler(payload);
        }
        catch
        {
            // A consumer's fault is never the supervisor's.
        }
    }
}
