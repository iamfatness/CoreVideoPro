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
        IShowEngineChild? child;
        lock (_gate)
        {
            if (_disposed) return;
            _disposed = true;
            _stopping = true;
            child = _child;
        }

        try { _lifetime.Cancel(); } catch { /* best effort */ }
        if (child is not null) TeardownChild(child, kill: true);
        RejectAll(new ObjectDisposedException(nameof(ShowEngineSupervisor)));
        _lifetime.Dispose();
        // _stdinGate is deliberately NOT disposed: an in-flight write may still be inside it, and a
        // disposed SemaphoreSlim turns that into an ObjectDisposedException on a shutdown path.
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
            // Cancelling the abandoned completion is what stops a timed-out request from leaving
            // behind a task nothing will ever complete (and, later, a JsonDocument nothing disposes).
            if (_pending.TryRemove(id, out var abandoned)) abandoned.Completion.TrySetCanceled();
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
        // A PARKED RecoverAsync SURVIVES BOTH StopAsync AND RestartAsync.
        //
        // StopAsync returns early when `_child` is already null (which it always is during a
        // backoff — TryClaimEnded detached it before BeginRecovery), so it neither cancels nor
        // awaits the recovery task; the parked `_delay` simply wakes up later and lands here. This
        // cheap pre-check is what makes "stop while parked" spawn NOTHING at all rather than spawn
        // and immediately kill. The authoritative guard is the attach block below — `_stopping` can
        // still be set, or a newer child attached, in the window between here and there.
        lock (_gate)
        {
            if (_stopping || _disposed) return false;
        }

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

        // Captured ONCE, before the reader loop starts: a child that is already dead at spawn can have
        // its reader loop hit EOF and dispose `scope` (see ReaderLoopAsync's finally) before this method
        // ever reads `scope.Token` below — a bare `scope.Token` read after that would throw
        // ObjectDisposedException out of StartAsync. `CancellationTokenSource.Token` returns the same
        // live token whether read before or after Dispose is called elsewhere, so capturing it up front
        // is always safe and this method must use ONLY this captured copy from here on.
        var token = scope.Token;

        // THE ATTACH IS ALSO A CLAIM ON BEING THE CURRENT CHILD.
        //
        // Without this guard a recovery parked on its backoff delay could wake AFTER a
        // RestartAsync had already started generation N+1 and overwrite `_child` with its own
        // spawn — stranding the newer engine process alive with its stdin held open by a
        // supervisor that no longer references it (an orphan that keeps running the show's
        // config, invisible to the operator). `_child` is ALWAYS null on the normal recovery path
        // (TryClaimEnded detached it before BeginRecovery ran) and on the normal start path
        // (StopAsync/Dispose/AbortStart all detach), so this fires ONLY on supersession.
        //
        // A superseded spawn owns its own teardown: its reader loop has not started, so nothing
        // else will ever dispose the child or the scope.
        bool superseded;
        lock (_gate)
        {
            superseded = _stopping || _disposed || _child is not null;
            if (!superseded)
            {
                _child = child;
                _childScope = scope;
                _handshakeSignal = handshakeSignal;
            }
        }

        if (superseded)
        {
            try { child.Kill(); } catch { /* best effort */ }
            try { child.Dispose(); } catch { /* best effort */ }
            try { scope.Cancel(); } catch { /* best effort */ }
            try { scope.Dispose(); } catch { /* best effort */ }
            return false;
        }

        _ = Task.Run(() => ReaderLoopAsync(child, scope), CancellationToken.None);
        _ = Task.Run(() => WatchExitAsync(child), CancellationToken.None);

        // The host writes an unsolicited handshake EVENT at startup. Only when that does not arrive in
        // time do we ask for one explicitly (which the host answers with a response AND another event).
        // child.Exited is in the race so a child that dies before announcing does not hold the start
        // for the full HandshakeTimeout — the exit watcher has already begun recovery by then.
        var wait = _delay(_options.HandshakeTimeout, token);
        await Task.WhenAny(handshakeSignal.Task, wait, child.Exited).ConfigureAwait(false);
        if (!handshakeSignal.Task.IsCompleted && !child.Exited.IsCompleted)
        {
            try
            {
                // HandshakeTimeout governs how long we wait for the UNSOLICITED announcement. Once we
                // are actively asking, the ordinary request timeout applies — a host that answers no
                // request at all is dead, and waiting another 15 s only delays the recovery.
                using var reply = await SendCoreAsync(child, "handshake", null, _options.RequestTimeout, token)
                    .ConfigureAwait(false);
                // Some hosts (and Task 6's fixtures) carry the manifest on the RESPONSE itself; the real
                // host follows it with the event, which the reader loop signals just as well.
                if (reply.RootElement.TryGetProperty("protocolVersion", out _))
                {
                    handshakeSignal.TrySetResult(reply.RootElement.Clone());
                }
                else
                {
                    // The real host (hostLoop.ts, the "handshake" request case) answers {id, ok:true}
                    // and THEN emits the handshake EVENT, so the manifest arrives on the FOLLOWING
                    // line. Ordinary request timing applies to that wait, not the announcement budget.
                    var second = _delay(_options.RequestTimeout, token);
                    await Task.WhenAny(handshakeSignal.Task, second).ConfigureAwait(false);
                    if (!handshakeSignal.Task.IsCompleted)
                    {
                        AbortStart(child, "show engine acknowledged the handshake but never sent one",
                            terminal: false);
                        return false;
                    }
                }
            }
            catch (Exception ex)
            {
                // A write error or a handshake TIMEOUT is transient - the engine may simply have been
                // slow to boot. It goes through the ordinary recovery path, not the terminal one.
                AbortStart(child, $"show engine handshake failed: {ex.Message}", terminal: false);
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
            AbortStart(child, "show engine did not handshake", terminal: false);
            return false;
        }

        var root = handshakeSignal.Task.Result;
        if (!ShowEngineProtocol.TryParseHandshake(root, out var handshake, out var error))
        {
            AbortStart(child, error ?? "unreadable show engine handshake", terminal: false);
            return false;
        }

        if (handshake.ProtocolVersion != ShowEngineProtocol.SupportedProtocolVersion)
        {
            // NOT a transient fault: a newer engine will never become compatible by restarting, so
            // this is terminal with no respawn, exactly like a config error (exit 78).
            AbortStart(child, $"unsupported protocol version {handshake.ProtocolVersion}", terminal: true);
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
        _ = Task.Run(() => HeartbeatLoopAsync(child, token), CancellationToken.None);
        return true;
    }

    /// <summary>End a start that did not reach Running. Only TWO conditions are
    /// <paramref name="terminal"/> - an unsupported protocol version and exit 78 - because neither can
    /// be fixed by restarting. Everything else (a handshake timeout, a write error, an unreadable
    /// manifest) is transient and takes the ordinary recovery path, so a slow-booting engine is never
    /// permanently written off.</summary>
    private void AbortStart(IShowEngineChild child, string error, bool terminal)
    {
        // THE EXIT CODE IS THE AUTHORITY OVER A CHILD THAT ALREADY DIED.
        //
        // Every non-terminal abort here rests on the ABSENCE of evidence ("it never handshook"), and a
        // child that exits before announcing is exactly what a config rejection looks like: exit 78 IS
        // "the engine read its config at startup and refused it". <see cref="WatchExitAsync"/> wakes off
        // the same completed TCS this method can observe, so whichever task claims first decides the
        // classification — and if this one won, the terminal exit-78 branch in
        // <see cref="OnChildEnded"/> would never run and a permanently broken config would respawn for
        // ever. So: withdraw, claim nothing, DETACH NOTHING (a detach here would make the exit
        // unclaimable and strand the supervisor in Starting), and let the exit path classify it.
        //
        // A TERMINAL abort is different and still wins: it rests on POSITIVE evidence we already read
        // off the wire (an unsupported protocolVersion), not on an absence.
        if (!terminal && child.Exited.IsCompleted)
        {
            RejectPendingFor(child, new InvalidOperationException(error));
            return;
        }

        var claimed = TryClaimEnded(child, out var request, out var scope);
        TeardownChild(child, scope, kill: true);
        RejectPendingFor(child, new InvalidOperationException(error));

        // Superseded (recovery already spawned a successor) or stopping: publish nothing.
        if (!claimed) return;

        if (terminal)
        {
            SetHealth(h => h with { State = ShowEngineState.Failed, LastError = error });
            return;
        }

        BeginRecovery(error, request);
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
                    lock (_gate)
                    {
                        if (ReferenceEquals(_child, child))
                        {
                            _handshakeSignal?.TrySetResult(doc.RootElement.Clone());
                        }
                    }

                    break;

                case ShowEngineProtocol.LineKind.Snapshot:
                    // A snapshot the host published just before it crashed is still in the pipe while
                    // the successor is already publishing NEWER ones. Delivering it would move the
                    // show state BACKWARDS, so a detached child's snapshots are dropped.
                    if (!IsCurrent(child)) break;
                    ShowEngineSnapshot snapshot;
                    try
                    {
                        snapshot = ShowEngineProtocol.ParseSnapshot(doc.RootElement);
                    }
                    catch (FormatException ex)
                    {
                        // A snapshot line with no `snapshot` node would otherwise publish
                        // default(JsonElement) onto ControlState.Ohg and throw at serialization
                        // time, far away from the line that caused it. Same treatment as a line
                        // that would not parse at all: warn, drop, keep reading.
                        Raise(LogReceived, new ShowEngineLogLine("warn",
                            $"malformed show engine snapshot: {ex.Message}"));
                        break;
                    }

                    Raise(SnapshotReceived, snapshot);
                    break;

                case ShowEngineProtocol.LineKind.HostCommand:
                    var command = ShowEngineProtocol.ParseHostCommand(doc.RootElement);
                    // A host command from an engine that is no longer ours would act on show state its
                    // sender no longer describes. Drop it, and COUNT it.
                    //
                    // CHILD IDENTITY IS THE PRIMARY GUARD, not the generation number:
                    // _currentGeneration is written only on a successful handshake and is never
                    // cleared, so all through Recovering + backoff + spawn it still names the DEAD
                    // generation - a stale command draining out of that child's pipe would sail
                    // straight through a number-only check. The generation compare stays as a second
                    // belt, for a LIVE child that echoes a generation we did not give it.
                    if (!IsCurrent(child) || command.Generation != Volatile.Read(ref _currentGeneration))
                    {
                        Interlocked.Increment(ref _staleHostCommandsDropped);
                        break;
                    }

                    Raise(HostCommandReceived, command);
                    break;

                case ShowEngineProtocol.LineKind.Log:
                    // Log lines are inert diagnostics, and the last of them are the whole reason a
                    // detached child is drained to EOF at all - so they are TAGGED rather than
                    // dropped. A consumer can always tell a dead generation's text from the live
                    // engine's, and can never mistake either for state.
                    var log = ShowEngineProtocol.ParseLog(doc.RootElement);
                    Raise(LogReceived, IsCurrent(child)
                        ? log
                        : log with { Message = $"[gen {child.Generation}, exited] {log.Message}" });
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
            catch (Exception ex) when (ex is OperationCanceledException or ObjectDisposedException)
            {
                return;   // teardown - the exit watcher owns whatever happens next
            }
            catch (Exception ex)
            {
                // An ordinary crash rejects this child's pendings with "Show engine exited." while a
                // ping is in flight, which lands right here. That is the exit path doing its job, not a
                // heartbeat fault, so it must not add a warn of its own.
                if (!IsCurrent(child)) return;

                // EVERY failed beat counts, not only a timeout. An engine that answers ok:false to a
                // ping surfaces here as an InvalidOperationException, and treating that as "stop
                // watching" would silently disable the watchdog against a host that is answering but
                // broken - the worst of both failure modes, and invisible.
                Raise(LogReceived, new ShowEngineLogLine("warn",
                    $"show engine heartbeat failed: {ex.Message}"));

                if (++missed < _options.MissedHeartbeatsBeforeHang) continue;

                // A child that accepts stdin but cannot answer it is WORSE than a crashed one: it
                // holds the show hostage with no exit code. Treat it as exit -1 and recover.
                Raise(LogReceived, new ShowEngineLogLine("warn",
                    $"show engine missed {missed} heartbeats - treating as a hang"));
                child.Kill();
                OnChildEnded(child, exitCode: -1);
                return;
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

    /// <summary>
    /// One child ended - crashed, exited, or was killed as a hang. THE DETACH IS THE CLAIM: the test
    /// and the detach happen in a single _gate acquisition, so exactly one caller can ever proceed.
    /// Two callers legitimately race here - the hang path calls this directly, and the Kill() it just
    /// issued also completes Exited and wakes <see cref="WatchExitAsync"/> into the same call - and a
    /// check-then-detach would let BOTH through: RestartCount would move by two, two RecoverAsync
    /// would run, and the second spawn would overwrite _child, leaving the first engine process
    /// running with nobody left to tear it down.
    /// </summary>
    private void OnChildEnded(IShowEngineChild child, int exitCode)
    {
        // The scope is deliberately NOT cancelled: this is the drain-to-EOF path, and the reader loop
        // owns disposing both it and the child when the pipe finally closes.
        if (!TryClaimEnded(child, out var request, out _)) return;

        // The process is already gone (or was just killed by the hang path), so the claim above only
        // DETACHED it. Its reader loop is deliberately left running to drain whatever is still in the
        // pipe; the child-identity guards in HandleLine are what keep those late lines from touching
        // the successor's state.
        RejectPendingFor(child, new InvalidOperationException("Show engine exited."));

        // Spec section 9: exit 78 is the engine rejecting its CONFIG. Restarting cannot fix a bad
        // config, so there is no backoff and no respawn - just a loud, terminal state that names the
        // log. Health is published ONCE, already terminal: never an intermediate Running-with-a-
        // bumped-restart-count that a consumer could latch.
        if (exitCode == 78)
        {
            SetHealth(h => h with
            {
                State = ShowEngineState.Failed,
                RestartCount = h.RestartCount + 1,
                LastCrashAt = _now(),
                LastError = "show engine rejected its config (exit 78) - see show-engine.log"
            });
            return;
        }

        BeginRecovery($"show engine exited with code {exitCode}", request);
    }

    /// <summary>Publish the crash exactly once and hand off to the backoff. The caller must already
    /// have claimed the child through <see cref="TryClaimEnded"/>.</summary>
    private void BeginRecovery(string error, ShowEngineSpawnRequest? request)
    {
        var crashedAt = _now();
        if (request is null)
        {
            SetHealth(h => h with
            {
                State = ShowEngineState.Failed,
                RestartCount = h.RestartCount + 1,
                LastCrashAt = crashedAt,
                LastError = "no spawn request to recover with"
            });
            return;
        }

        SetHealth(h => h with
        {
            State = ShowEngineState.Recovering,
            RestartCount = h.RestartCount + 1,
            LastCrashAt = crashedAt,
            LastError = error
        });

        _ = Task.Run(() => RecoverAsync(request), CancellationToken.None);
    }

    /// <summary>Atomically test-and-detach: returns true exactly once per child, and only while the
    /// supervisor is neither stopping nor disposed.</summary>
    private bool TryClaimEnded(
        IShowEngineChild child, out ShowEngineSpawnRequest? request, out CancellationTokenSource? scope)
    {
        lock (_gate)
        {
            request = _request;
            scope = null;
            if (_stopping || _disposed || !ReferenceEquals(_child, child)) return false;
            scope = _childScope;
            _child = null;
            _childScope = null;
            return true;
        }
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
    private void TeardownChild(IShowEngineChild child, bool kill) =>
        TeardownChild(child, DetachChild(child), kill);

    /// <summary>Forced teardown for a caller that has ALREADY detached the child — it must hand the
    /// scope it claimed back in, because <see cref="DetachChild"/> would now return null and the reader
    /// loop would be left running against a child nobody owns.</summary>
    private void TeardownChild(IShowEngineChild child, CancellationTokenSource? scope, bool kill)
    {
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

    /// <summary>True while <paramref name="child"/> is still the supervisor's current child.</summary>
    private bool IsCurrent(IShowEngineChild child)
    {
        lock (_gate) return ReferenceEquals(_child, child);
    }

    /// <summary>Raise an event without letting a subscriber's exception take the reader loop - or the
    /// process - with it (this repo's queued-callback fail-fast rule, applied at the source). The
    /// swallowed fault is REPORTED on <see cref="LogReceived"/>, except when LogReceived is itself the
    /// handler that threw, which would recurse.</summary>
    private void Raise<T>(Action<T>? handler, T payload)
    {
        if (handler is null) return;
        try
        {
            handler(payload);
        }
        catch (Exception ex)
        {
            if (typeof(T) == typeof(ShowEngineLogLine)) return;
            Raise(LogReceived, new ShowEngineLogLine("error",
                $"a show engine {typeof(T).Name} subscriber threw: {ex.Message}"));
        }
    }
}
