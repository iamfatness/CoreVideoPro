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
        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        var delays = new DelayController();
        delays.SetImmediate(Options.HandshakeTimeout);   // no unsolicited handshake arrives -> fall back
        factory.Configure = child => child.Responder = line =>
            FakeShowEngineChild.TypeOf(line) == "handshake"
                ? new[] { TestLines.HandshakeResponse(IdOf(line), child.Generation) }
                : Array.Empty<string>();

        using var sup = NewSupervisor(factory, delays);
        await sup.StartAsync(Request, CancellationToken.None);

        Assert.Equal(ShowEngineState.Running, sup.Health.State);
        Assert.Contains(factory.Child(1).Written, l => FakeShowEngineChild.TypeOf(l) == "handshake");
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

        await WaitUntil(() => factory.SpawnCount == 2, "the hang to trigger a respawn");
        Assert.True(factory.Child(1).Killed);
        Assert.Equal(ShowEngineState.Recovering, sup.Health.State);
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
    public async Task SendAsync_ThrowsWhenNotRunning()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);

        await Assert.ThrowsAsync<InvalidOperationException>(
            () => sup.SendAsync("ping", null, CancellationToken.None));
    }

    private static string IdOf(string line)
    {
        using var doc = JsonDocument.Parse(line);
        return doc.RootElement.GetProperty("id").GetString()!;
    }
}
