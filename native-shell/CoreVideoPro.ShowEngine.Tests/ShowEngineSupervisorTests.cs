using System.Text.Json;
using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

public sealed class ShowEngineSupervisorTests
{
    private static readonly ShowEngineSupervisorOptions Options = new();

    private static readonly ShowEngineSpawnRequest Request = new(
        NodeExe: @"C:\node\node.exe",
        EntryScript: @"C:\app\show-engine\dist\host\main.js",
        ConfigPath: @"C:\cfg\ohg-show-config.json",
        WorkingDirectory: @"C:\app\show-engine",
        Environment: new Dictionary<string, string>());

    private static readonly DateTimeOffset Clock = new(2026, 9, 7, 12, 0, 0, TimeSpan.Zero);

    private static ShowEngineSupervisor NewSupervisor(FakeChildFactory factory, DelayController delays,
        ShowEngineRestartPolicy? policy = null, ShowEngineSupervisorOptions? options = null) =>
        new(factory, policy ?? new ShowEngineRestartPolicy(), delays.DelayAsync, () => Clock, options ?? Options);

    /// <summary>Bounded async poll — the only wait in these tests; no production delay is ever real.</summary>
    private static async Task WaitUntil(Func<bool> condition, string what, int timeoutMs = 5000)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        while (!condition())
        {
            if (Environment.TickCount64 > deadline) Assert.Fail("timed out waiting for " + what);
            await Task.Delay(1).ConfigureAwait(false);
        }
    }

    [Fact]
    public async Task Start_AcceptsTheUnsolicitedHandshake_AndBecomesRunning()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();   // everything parks: no timeout, no heartbeat, no fallback
        using var sup = NewSupervisor(factory, delays);

        ShowEngineHandshake? seen = null;
        sup.Handshaken += h => seen = h;

        await sup.StartAsync(Request, CancellationToken.None);

        Assert.Equal(ShowEngineState.Running, sup.Health.State);
        Assert.Equal(1, sup.Health.Generation);
        Assert.Equal(0, sup.Health.RestartCount);
        Assert.NotNull(seen);
        Assert.Equal(1, seen!.ProtocolVersion);
        Assert.Equal("0.1.0", seen.EngineVersion);
        Assert.Equal(2, seen.Actions.Count);
        Assert.Equal("ohg.look.set", seen.Actions[1].Id);
        Assert.Equal("lookId", seen.Actions[1].Params[0].Name);
        Assert.Equal(2, seen.FieldTemplates.Count);
        Assert.Equal("wide", seen.Fields["ohg/look"].GetString());
        Assert.Equal(10, seen.Snapshot.GetProperty("capacity").GetInt32());

        // no explicit handshake request was needed
        Assert.DoesNotContain(factory.Child(1).Written, l => FakeShowEngineChild.TypeOf(l) == "handshake");
        Assert.Equal(1, factory.SpawnCount);
    }

    [Fact]
    public async Task Start_FallsBackToAnExplicitHandshakeRequest()
    {
        // The REAL host shape (show-engine/src/host/hostLoop.ts, the "handshake" request case): it
        // answers {id, ok:true} and THEN emits the handshake EVENT on the following line. The
        // supervisor must ride out that second line rather than treating the bare ack as the manifest.
        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        var delays = new DelayController();
        delays.SetImmediate(Options.HandshakeTimeout);   // no unsolicited handshake arrives -> fall back
        factory.Configure = child => child.Responder = line =>
            FakeShowEngineChild.TypeOf(line) == "handshake"
                ? new[] { TestLines.OkResponse(IdOf(line)), TestLines.HandshakeEvent(child.Generation) }
                : Array.Empty<string>();

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);

        Assert.Equal(ShowEngineState.Running, sup.Health.State);
        Assert.Equal(1, sup.Health.Generation);
        Assert.Contains(factory.Child(1).Written, l => FakeShowEngineChild.TypeOf(l) == "handshake");
        Assert.Equal(1, factory.SpawnCount);
    }

    [Fact]
    public async Task HandshakeTimeout_Recovers_RatherThanFailingTerminally()
    {
        // A handshake that never lands is TRANSIENT (a slow-booting engine), unlike a version mismatch
        // or exit 78. It must respawn through the policy, not be written off.
        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        var delays = new DelayController();
        delays.SetImmediate(Options.HandshakeTimeout, Options.RequestTimeout);
        var policy = new OneRetryPolicy();

        var states = new List<ShowEngineState>();
        using var sup = NewSupervisor(factory, delays, policy);
        sup.HealthChanged += h => { lock (states) states.Add(h.State); };

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Failed, "the retry budget to run out");

        Assert.Equal(2, factory.SpawnCount);                       // it RESPAWNED, so it was not terminal
        // The claimed scope must reach TeardownChild, or the aborted child's reader loop outlives it.
        Assert.True(factory.Child(1).Disposed);
        lock (states) Assert.Contains(ShowEngineState.Recovering, states);
        Assert.Equal(2, sup.Health.RestartCount);
        Assert.Contains("press Restart", sup.Health.LastError);
    }

    [Fact]
    public async Task StaleHostCommand_DuringTheRecoveryWindow_IsDropped()
    {
        // The window the generation NUMBER cannot cover: gen 1 has died, gen 2 is spawned but has not
        // handshaked, so _currentGeneration still reads 1 — a gen-1 command draining out of the dead
        // child would pass a number-only check. Child identity is what closes it.
        var factory = new FakeChildFactory { PreloadHandshake = g => g == 1 };
        var delays = new DelayController();          // HandshakeTimeout parks: gen 2 never handshakes
        delays.SetImmediate(TimeSpan.FromSeconds(1)); // let the real policy's first backoff fire at once
        using var sup = NewSupervisor(factory, delays);

        var commands = new List<ShowEngineHostCommand>();
        sup.HostCommandReceived += c => { lock (commands) commands.Add(c); };

        await sup.StartAsync(Request, CancellationToken.None);
        var gen1 = factory.Child(1);

        gen1.SignalExit(3);                          // stdout stays open: the pipe is still draining
        await WaitUntil(() => factory.SpawnCount == 2, "the successor to be spawned");
        Assert.Equal(ShowEngineState.Recovering, sup.Health.State);
        Assert.Equal(1, sup.Health.Generation);      // still names the DEAD generation

        gen1.Push(TestLines.HostCommandEvent(generation: 1, seq: 4, name: "cut"));
        await WaitUntil(() => sup.StaleHostCommandsDropped == 1, "the mid-recovery command to be dropped");
        Assert.Empty(commands);
    }

    [Fact]
    public async Task StaleChildSnapshots_AreDropped_AndItsLogsAreTagged()
    {
        var factory = new FakeChildFactory { PreloadHandshake = g => g == 1 };
        var delays = new DelayController();
        delays.SetImmediate(TimeSpan.FromSeconds(1)); // let the real policy's first backoff fire at once
        using var sup = NewSupervisor(factory, delays);

        var snapshots = new List<ShowEngineSnapshot>();
        var logs = new List<ShowEngineLogLine>();
        sup.SnapshotReceived += x => { lock (snapshots) snapshots.Add(x); };
        sup.LogReceived += l => { lock (logs) logs.Add(l); };

        await sup.StartAsync(Request, CancellationToken.None);
        var gen1 = factory.Child(1);
        gen1.SignalExit(3);
        await WaitUntil(() => factory.SpawnCount == 2, "the successor to be spawned");

        gen1.Push(TestLines.SnapshotEvent(1, 99));
        gen1.Push(TestLines.LogEvent("error", "dying words"));
        await WaitUntil(() => { lock (logs) return logs.Any(l => l.Message.Contains("dying words")); },
            "the dead child's log line");

        // The stale snapshot would have moved show state BACKWARDS, so it is dropped...
        lock (snapshots) Assert.Empty(snapshots);
        // ...but its log line still arrives, TAGGED with the generation that is gone.
        var tagged = logs.Single(l => l.Message.Contains("dying words"));
        Assert.Equal("[gen 1, exited] dying words", tagged.Message);
    }

    [Fact]
    public async Task UnsupportedProtocolVersion_IsFailed_WithoutRestart()
    {
        var factory = new FakeChildFactory { HandshakeProtocolVersion = 2 };
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);

        await sup.StartAsync(Request, CancellationToken.None);

        Assert.Equal(ShowEngineState.Failed, sup.Health.State);
        Assert.Contains("protocol version 2", sup.Health.LastError);
        Assert.Equal(1, factory.SpawnCount);
        Assert.True(factory.Child(1).Killed);
    }

    [Fact]
    public async Task Send_CorrelatesById_AndTimesOut()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        factory.Configure = child => child.Responder = line =>
            FakeShowEngineChild.TypeOf(line) == "ping"
                ? new[] { TestLines.OkResponse(IdOf(line), "\"revision\":9") }
                : Array.Empty<string>();

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);

        using (var reply = await sup.SendAsync("ping", null, CancellationToken.None))
        {
            Assert.Equal(9, reply.RootElement.GetProperty("revision").GetInt64());
        }

        // Now nothing answers, and the injected delay for RequestTimeout completes at once.
        factory.Child(1).Responder = _ => Array.Empty<string>();
        delays.SetImmediate(Options.RequestTimeout);
        await Assert.ThrowsAsync<TimeoutException>(
            () => sup.SendAsync("capacity", new { capacity = 10 }, CancellationToken.None));
    }

    [Fact]
    public async Task StaleGeneration_ResponsesAndHostCommands_AreDropped()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        delays.SetImmediate(TimeSpan.FromSeconds(1)); // let the real policy's first backoff fire at once
        using var sup = NewSupervisor(factory, delays);

        var commands = new List<ShowEngineHostCommand>();
        sup.HostCommandReceived += c => { lock (commands) commands.Add(c); };

        await sup.StartAsync(Request, CancellationToken.None);
        var gen1 = factory.Child(1);

        // gen 1 dies; its stdout stays readable so its reader loop can still see the stale lines below
        gen1.SignalExit(3);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running && sup.Health.Generation == 2,
            "generation 2 to handshake");
        Assert.Equal(2, factory.SpawnCount);

        // a request issued on the CURRENT child, left unanswered
        var pending = sup.SendAsync("capacity", new { capacity = 10 }, CancellationToken.None);
        await WaitUntil(() => factory.Child(2).IdOfWritten("capacity") is not null, "the capacity request write");
        var pendingId = factory.Child(2).IdOfWritten("capacity")!;

        // the STALE child answers it, and emits a stale host command
        gen1.Push(TestLines.OkResponse(pendingId, "\"revision\":42"));
        gen1.Push(TestLines.HostCommandEvent(generation: 1, seq: 7, name: "cut"));
        await WaitUntil(() => sup.StaleHostCommandsDropped == 1, "the stale host command to be dropped");

        Assert.False(pending.IsCompleted);
        Assert.Empty(commands);

        // the CURRENT generation host command does fire
        factory.Child(2).Push(TestLines.HostCommandEvent(2, 8, "assignSlot", "[1,\"p9\"]"));
        await WaitUntil(() => { lock (commands) return commands.Count == 1; }, "the live host command");
        Assert.Equal(1, sup.StaleHostCommandsDropped);
        Assert.Equal(8, commands[0].Seq);
        Assert.Equal("assignSlot", commands[0].Name);
        Assert.Equal(2, commands[0].Args.GetArrayLength());
    }

    [Fact]
    public async Task Exit78_IsFailedImmediately_NoDelay()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);

        await sup.StartAsync(Request, CancellationToken.None);
        factory.Child(1).Complete(78);

        await WaitUntil(() => sup.Health.State == ShowEngineState.Failed, "exit 78 to fail the engine");
        Assert.Contains("exit 78", sup.Health.LastError);
        Assert.Equal(1, factory.SpawnCount);
        // the stub policy returns TimeSpan.Zero, so any backoff would have recorded a Zero delay
        Assert.DoesNotContain(TimeSpan.Zero, delays.Recorded);
        Assert.NotNull(sup.Health.LastCrashAt);
    }

    [Fact]
    public async Task Exit78_BeforeAnyHandshake_IsStillTerminal()
    {
        // The config-rejection shape: the engine reads its config at startup, refuses it, and dies
        // WITHOUT ever announcing. The missing handshake is the weaker evidence; the exit code is the
        // authority, so this must be terminal, not an endless respawn of a permanently broken config.
        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        factory.Configure = child => child.Complete(78);
        var delays = new DelayController();

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Failed, "the pre-handshake exit 78");

        Assert.Contains("exit 78", sup.Health.LastError);
        Assert.Equal(1, factory.SpawnCount);
        Assert.DoesNotContain(TimeSpan.Zero, delays.Recorded);   // no backoff was taken
        Assert.Equal(1, sup.Health.RestartCount);
    }

    [Theory]
    [InlineData(1)] [InlineData(2)] [InlineData(3)] [InlineData(4)] [InlineData(5)]
    [InlineData(6)] [InlineData(7)] [InlineData(8)] [InlineData(9)] [InlineData(10)]
    [InlineData(11)] [InlineData(12)] [InlineData(13)] [InlineData(14)] [InlineData(15)]
    [InlineData(16)] [InlineData(17)] [InlineData(18)] [InlineData(19)] [InlineData(20)]
    public async Task PreHandshakeDeath_NeverThrowsOutOfStart(int iteration)
    {
        // Regression for the Task 6 re-review finding: a child already dead at spawn can have its
        // reader loop hit EOF and dispose the child's CancellationTokenSource BEFORE
        // SpawnAndHandshakeAsync ever reads `scope.Token` (for the handshake-timeout delay and the
        // heartbeat Task.Run) — a race that, uncaught, throws ObjectDisposedException out of
        // StartAsync. `iteration` (1..20) just runs this repeatedly so a rare race is not missed by luck.
        _ = iteration;
        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        factory.Configure = child => child.Complete(78);
        var delays = new DelayController();

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);   // must never throw
        await WaitUntil(() => sup.Health.State == ShowEngineState.Failed, "the pre-handshake exit 78");

        Assert.Contains("exit 78", sup.Health.LastError);
    }

    [Fact]
    public async Task Recovery_UsesThePolicyDelay_AndFailsWhenExhausted()
    {
        // A real policy, budget 2: two respawns are granted (1s, then 2s), a third crash exhausts it.
        // HeartbeatInterval is pinned well away from 1s/2s/4s/8s so a heartbeat park can never be
        // mistaken for a backoff entry when both land in delays.Recorded.
        var options = new ShowEngineSupervisorOptions { HeartbeatInterval = TimeSpan.FromMinutes(5) };
        var factory = new FakeChildFactory();
        factory.Configure = child => child.Responder = line =>
            FakeShowEngineChild.TypeOf(line) == "ping"
                ? new[] { TestLines.OkResponse(IdOf(line)) }
                : Array.Empty<string>();
        var delays = new DelayController();
        delays.SetImmediate(TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2));
        var policy = new ShowEngineRestartPolicy(maxConsecutiveFailures: 2);

        using var sup = NewSupervisor(factory, delays, policy, options);
        await sup.StartAsync(Request, CancellationToken.None);
        Assert.Equal(ShowEngineState.Running, sup.Health.State);

        factory.Child(1).Complete(1);
        await WaitUntil(() => factory.SpawnCount == 2 && sup.Health.State == ShowEngineState.Running,
            "the first respawn to handshake");

        factory.Child(2).Complete(1);
        await WaitUntil(() => factory.SpawnCount == 3 && sup.Health.State == ShowEngineState.Running,
            "the second respawn to handshake");

        factory.Child(3).Complete(1);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Failed, "the budget to exhaust");

        var backoffSized = delays.Recorded
            .Where(d => d == TimeSpan.FromSeconds(1) || d == TimeSpan.FromSeconds(2))
            .ToList();
        Assert.Equal(new[] { TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(2) }, backoffSized);
        Assert.DoesNotContain(TimeSpan.FromSeconds(4), delays.Recorded);
        Assert.DoesNotContain(TimeSpan.FromSeconds(8), delays.Recorded);

        Assert.Equal(3, factory.SpawnCount);
        Assert.Equal(ShowEngineState.Failed, sup.Health.State);
        Assert.Equal(3, sup.Health.RestartCount);

        // ------------------------------------------------------------------ healthy-reset, isolated
        // A second, independent scenario (fresh supervisor + policy, mutable clock) proves the OTHER
        // half of the contract: a heartbeat while Running clears the consecutive-failure count once the
        // current running spell has lasted 60s, even though the count above was earned by real crashes.
        var now = Clock;
        var factory2 = new FakeChildFactory();
        factory2.Configure = child => child.Responder = line =>
            FakeShowEngineChild.TypeOf(line) == "ping"
                ? new[] { TestLines.OkResponse(IdOf(line)) }
                : Array.Empty<string>();
        var delays2 = new DelayController();
        delays2.SetImmediate(TimeSpan.FromSeconds(1));   // first backoff fires at once
        var policy2 = new ShowEngineRestartPolicy();
        var options2 = new ShowEngineSupervisorOptions { HeartbeatInterval = TimeSpan.FromSeconds(30) };

        using var sup2 = new ShowEngineSupervisor(factory2, policy2, delays2.DelayAsync, () => now, options2);
        await sup2.StartAsync(Request, CancellationToken.None);
        Assert.Equal(ShowEngineState.Running, sup2.Health.State);

        factory2.Child(1).Complete(1);
        await WaitUntil(() => factory2.SpawnCount == 2 && sup2.Health.State == ShowEngineState.Running,
            "the respawn to handshake");
        Assert.Equal(1, policy2.ConsecutiveFailures);

        await WaitUntil(() => delays2.ParkedCount(options2.HeartbeatInterval) > 0, "the first heartbeat wait");
        now += TimeSpan.FromSeconds(30);
        delays2.Release(options2.HeartbeatInterval);

        await WaitUntil(() => delays2.ParkedCount(options2.HeartbeatInterval) > 0, "the second heartbeat wait");
        now += TimeSpan.FromSeconds(31);   // 61s total since RecordRunning — crosses the 60s reset
        delays2.Release(options2.HeartbeatInterval);

        await WaitUntil(() => policy2.ConsecutiveFailures == 0, "the healthy reset");
    }

    [Fact]
    public async Task OrdinaryExit_BeforeAnyHandshake_Recovers()
    {
        // The sibling classification on the same pre-handshake path: an ordinary exit code IS
        // transient, so it respawns. Both outcomes are pinned so neither can drift into the other.
        var factory = new FakeChildFactory { PreloadHandshake = g => g == 2 };
        factory.Configure = child => { if (child.Generation == 1) child.Complete(1); };
        var delays = new DelayController();
        delays.SetImmediate(TimeSpan.FromSeconds(1)); // let the real policy's first backoff fire at once

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running, "the successor to handshake");

        Assert.Equal(2, factory.SpawnCount);
        Assert.Equal(2, sup.Health.Generation);
        Assert.Equal(1, sup.Health.RestartCount);
    }

    [Fact]
    public async Task Hang_TwoMissedHeartbeats_TriggersRecovery()
    {
        // gen 2 never handshakes, so the supervisor is observably still Recovering after the respawn.
        var factory = new FakeChildFactory { PreloadHandshake = g => g == 1 };
        var delays = new DelayController();
        delays.SetImmediate(Options.RequestTimeout);   // every ping times out at once
        factory.Configure = child => child.Responder = _ => Array.Empty<string>();

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);

        // drive the heartbeat MissedHeartbeatsBeforeHang times
        for (var beat = 0; beat < Options.MissedHeartbeatsBeforeHang; beat++)
        {
            await WaitUntil(() => delays.ParkedCount(Options.HeartbeatInterval) > 0, "heartbeat wait");
            delays.Release(Options.HeartbeatInterval);
        }

        // the hang trips the real policy's first backoff (1s, same value as HeartbeatInterval — the
        // dead child's heartbeat loop has already returned, so this park can only be the backoff)
        await WaitUntil(() => delays.ParkedCount(TimeSpan.FromSeconds(1)) > 0, "the backoff wait");
        delays.Release(TimeSpan.FromSeconds(1));

        await WaitUntil(() => factory.SpawnCount == 2, "the hang to trigger a respawn");
        Assert.True(factory.Child(1).Killed);
        Assert.Equal(ShowEngineState.Recovering, sup.Health.State);
        // Kill() also completes Exited, so the hang path and the exit watcher both reach OnChildEnded.
        // Exactly ONE may claim the child: a second claim would double the count and orphan a process.
        Assert.Equal(1, sup.Health.RestartCount);
        Assert.Equal(2, factory.SpawnCount);
    }

    [Fact]
    public async Task PingAnsweredNotOk_StillTripsTheHangWatchdog()
    {
        // A host that ANSWERS but answers ok:false used to be the worst case: the heartbeat's bare
        // catch returned, silently disabling the watchdog against a broken-but-responsive engine.
        var factory = new FakeChildFactory { PreloadHandshake = g => g == 1 };
        var delays = new DelayController();
        factory.Configure = child => child.Responder = line =>
            FakeShowEngineChild.TypeOf(line) == "ping"
                ? new[] { TestLines.ErrorResponse(IdOf(line), "engine is wedged") }
                : Array.Empty<string>();

        var logs = new List<ShowEngineLogLine>();
        using var sup = NewSupervisor(factory, delays);
        sup.LogReceived += l => { lock (logs) logs.Add(l); };

        await sup.StartAsync(Request, CancellationToken.None);

        for (var beat = 0; beat < Options.MissedHeartbeatsBeforeHang; beat++)
        {
            await WaitUntil(() => delays.ParkedCount(Options.HeartbeatInterval) > 0, "heartbeat wait");
            delays.Release(Options.HeartbeatInterval);
        }

        // the hang trips the real policy's first backoff (1s, same value as HeartbeatInterval — the
        // dead child's heartbeat loop has already returned, so this park can only be the backoff)
        await WaitUntil(() => delays.ParkedCount(TimeSpan.FromSeconds(1)) > 0, "the backoff wait");
        delays.Release(TimeSpan.FromSeconds(1));

        await WaitUntil(() => factory.SpawnCount == 2, "the failed beats to trigger a respawn");
        Assert.True(factory.Child(1).Killed);
        Assert.Equal(ShowEngineState.Recovering, sup.Health.State);
        Assert.Equal(1, sup.Health.RestartCount);      // claimed exactly once, never twice
        lock (logs) Assert.Contains(logs, l => l.Message.Contains("engine is wedged"));
    }

    [Fact]
    public async Task OrdinaryCrash_WithAPingInFlight_AddsNoHeartbeatWarning()
    {
        // The crash rejects this child's pendings with "Show engine exited." — and the in-flight ping is
        // one of them, so the rejection surfaces inside the heartbeat's catch. That is the exit path
        // doing its job; reporting it as a heartbeat fault would put a spurious warning in the log of
        // every ordinary crash, right where an operator goes looking for the real cause.
        var factory = new FakeChildFactory { PreloadHandshake = g => g == 1 };
        var delays = new DelayController();
        factory.Configure = child => child.Responder = _ => Array.Empty<string>();

        var logs = new List<ShowEngineLogLine>();
        using var sup = NewSupervisor(factory, delays);
        sup.LogReceived += l => { lock (logs) logs.Add(l); };

        await sup.StartAsync(Request, CancellationToken.None);
        var gen1 = factory.Child(1);

        // one heartbeat beat, answered by nobody and never timing out: the ping is now pending
        await WaitUntil(() => delays.ParkedCount(Options.HeartbeatInterval) > 0, "the heartbeat wait");
        delays.Release(Options.HeartbeatInterval);
        await WaitUntil(() => gen1.IdOfWritten("ping") is not null, "the ping to be written");

        gen1.SignalExit(3);

        // the crash trips the real policy's first backoff (1s, same value as HeartbeatInterval — the
        // dead child's heartbeat loop has already returned, so this park can only be the backoff)
        await WaitUntil(() => delays.ParkedCount(TimeSpan.FromSeconds(1)) > 0, "the backoff wait");
        delays.Release(TimeSpan.FromSeconds(1));

        await WaitUntil(() => factory.SpawnCount == 2, "the crash to be recovered");

        lock (logs) Assert.DoesNotContain(logs, l => l.Message.Contains("heartbeat failed"));
        Assert.Equal(1, sup.Health.RestartCount);
    }

    [Fact]
    public async Task Stop_SendsShutdown_ThenIsStopped()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        factory.Configure = child => child.Responder = line =>
        {
            if (FakeShowEngineChild.TypeOf(line) != "shutdown") return Array.Empty<string>();
            var reply = TestLines.OkResponse(IdOf(line));
            child.ExitAfterWrite = 0;
            return new[] { reply };
        };

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);

        await sup.StopAsync();

        Assert.Equal(ShowEngineState.Stopped, sup.Health.State);
        Assert.Equal("shutdown", FakeShowEngineChild.TypeOf(factory.Child(1).Written[^1]));
        Assert.False(factory.Child(1).Killed);
        Assert.Equal(1, factory.SpawnCount);
    }

    [Fact]
    public async Task Snapshots_And_Logs_AreRaised_AndMalformedLinesWarn()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);

        var snapshots = new List<ShowEngineSnapshot>();
        var logs = new List<ShowEngineLogLine>();
        sup.SnapshotReceived += s => { lock (snapshots) snapshots.Add(s); };
        sup.LogReceived += l => { lock (logs) logs.Add(l); };

        await sup.StartAsync(Request, CancellationToken.None);
        factory.Child(1).Push(TestLines.SnapshotEvent(1, 12));
        factory.Child(1).Push(TestLines.LogEvent("warn", "restore skipped"));
        factory.Child(1).Push("{not json");

        await WaitUntil(() => { lock (snapshots) return snapshots.Count == 1; }, "a snapshot event");
        await WaitUntil(() => { lock (logs) return logs.Count == 2; }, "a log line and a malformed warning");

        Assert.Equal(12, snapshots[0].Revision);
        Assert.Equal("tight", snapshots[0].Fields["ohg/look"].GetString());
        Assert.Equal("warn", logs[0].Level);
        Assert.Equal("restore skipped", logs[0].Message);
        Assert.Equal("warn", logs[1].Level);
        Assert.Contains("malformed", logs[1].Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task RestartDuringBackoff_KillsTheSupersededRecoverySpawn()
    {
        // THE ORPHAN. A crash parks RecoverAsync on its backoff delay. StopAsync returns EARLY
        // during a backoff (`_child` is already null — TryClaimEnded detached it), so an operator
        // Restart neither cancels nor awaits that parked task: it runs StopAsync, then StartAsync,
        // and generation 2 is live. When the parked delay finally wakes, the recovery spawns
        // generation 3 and — without the attach guard — overwrites `_child` with it, leaving
        // generation 2's node.exe running with its stdin held open by nobody.
        var options = new ShowEngineSupervisorOptions { HeartbeatInterval = TimeSpan.FromMinutes(5) };
        var factory = new FakeChildFactory();
        var delays = new DelayController();          // the 1s backoff PARKS: that is the window
        var backoff = ShowEngineRestartPolicy.Delays[0];

        var handshakes = 0;
        using var sup = NewSupervisor(factory, delays, options: options);
        sup.Handshaken += _ => Interlocked.Increment(ref handshakes);

        await sup.StartAsync(Request, CancellationToken.None);
        Assert.Equal(ShowEngineState.Running, sup.Health.State);

        factory.Child(1).Complete(1);
        await WaitUntil(() => delays.ParkedCount(backoff) == 1, "the recovery to park on its backoff");
        Assert.Equal(ShowEngineState.Recovering, sup.Health.State);

        // The operator restarts while the recovery is still parked.
        await sup.RestartAsync(Request, CancellationToken.None);
        Assert.Equal(ShowEngineState.Running, sup.Health.State);
        Assert.Equal(2, sup.Health.Generation);
        Assert.Equal(2, factory.SpawnCount);

        // ... and only NOW does the parked recovery wake up.
        Assert.Equal(1, delays.Release(backoff));
        await WaitUntil(() => factory.SpawnCount == 3, "the superseded recovery to spawn");

        // Its child is killed and disposed by the spawn path itself — no reader loop ever started
        // for it, so nothing else would have.
        await WaitUntil(() => factory.Child(3).Killed && factory.Child(3).Disposed,
            "the superseded spawn to be killed and disposed");

        // Generation 2 is untouched: still current, still Running, and its handshake is the last
        // one accepted (child 3's preloaded handshake was never read).
        Assert.Equal(ShowEngineState.Running, sup.Health.State);
        Assert.Equal(2, sup.Health.Generation);
        Assert.False(factory.Child(2).Killed);
        Assert.False(factory.Child(2).Disposed);
        Assert.Equal(2, Volatile.Read(ref handshakes));
    }

    [Fact]
    public async Task StopDuringBackoff_NeverSpawns()
    {
        // The sibling of the case above: a stop while parked must not spawn AT ALL — not spawn and
        // then kill. A spawned-then-killed child is a real node.exe that briefly read the show
        // config and wrote to the engine log after the operator stopped the engine.
        var options = new ShowEngineSupervisorOptions { HeartbeatInterval = TimeSpan.FromMinutes(5) };
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        var backoff = ShowEngineRestartPolicy.Delays[0];

        using var sup = NewSupervisor(factory, delays, options: options);
        await sup.StartAsync(Request, CancellationToken.None);

        factory.Child(1).Complete(1);
        await WaitUntil(() => delays.ParkedCount(backoff) == 1, "the recovery to park on its backoff");

        await sup.StopAsync();
        Assert.Equal(ShowEngineState.Stopped, sup.Health.State);

        Assert.Equal(1, delays.Release(backoff));

        // Give the woken recovery every chance to spawn; the assertion is that it does not.
        await Task.Delay(100);
        Assert.Equal(1, factory.SpawnCount);
        Assert.Equal(ShowEngineState.Stopped, sup.Health.State);
    }

    [Fact]
    public async Task SendAsync_ThrowsWhenNotRunning()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => sup.SendAsync("ping", null, CancellationToken.None));
    }

    /// <summary>Grants exactly one respawn, then gives up — so a never-handshaking engine terminates
    /// the test instead of hot-looping through the stub policy's unconditional TimeSpan.Zero.</summary>
    private sealed class OneRetryPolicy : ShowEngineRestartPolicy
    {
        private int _calls;

        public override TimeSpan? NextDelay(DateTimeOffset now) =>
            Interlocked.Increment(ref _calls) == 1 ? TimeSpan.Zero : null;

        public override int ConsecutiveFailures => Volatile.Read(ref _calls);
    }

    private static string IdOf(string line)
    {
        using var doc = JsonDocument.Parse(line);
        return doc.RootElement.GetProperty("id").GetString()!;
    }
}
