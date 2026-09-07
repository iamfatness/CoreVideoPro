using System.Text.Json;
using CoreVideoPro.Control;
using CoreVideoPro.ShowEngine;
using Xunit;

namespace CoreVideoPro.ShowEngine.Tests;

/// <summary>
/// Drives a REAL <see cref="ShowEngineSupervisor"/> over the Task 6 fake child (same rig as
/// <see cref="ShowEngineSupervisorTests"/>) — the bridge itself is never mocked, and neither is the
/// supervisor underneath it.
/// </summary>
public sealed class ShowEngineBridgeTests
{
    private static readonly ShowEngineSupervisorOptions Options = new();

    private static readonly ShowEngineSpawnRequest Request = new(
        NodeExe: @"C:\node\node.exe",
        EntryScript: @"C:\app\show-engine\dist\host\main.js",
        ConfigPath: @"C:\cfg\ohg-show-config.json",
        WorkingDirectory: @"C:\app\show-engine",
        Environment: new Dictionary<string, string>());

    private static readonly DateTimeOffset Clock = new(2026, 9, 7, 12, 0, 0, TimeSpan.Zero);

    private static ShowEngineSupervisor NewSupervisor(FakeChildFactory factory, DelayController delays) =>
        new(factory, new ShowEngineRestartPolicy(), delays.DelayAsync, () => Clock, Options);

    private static async Task WaitUntil(Func<bool> condition, string what, int timeoutMs = 5000)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        while (!condition())
        {
            if (Environment.TickCount64 > deadline) Assert.Fail("timed out waiting for " + what);
            await Task.Delay(1).ConfigureAwait(false);
        }
    }

    private static string IdOf(string line)
    {
        using var doc = JsonDocument.Parse(line);
        return doc.RootElement.GetProperty("id").GetString()!;
    }

    /// <summary>Wires the fake child's "invoke" requests to a scripted per-action result table.</summary>
    private static void RespondToInvoke(FakeChildFactory factory, IReadOnlyDictionary<string, string> resultJsonByAction)
    {
        factory.Configure = child => child.Responder = line =>
        {
            using var doc = JsonDocument.Parse(line);
            if (doc.RootElement.GetProperty("type").GetString() != "invoke") return Array.Empty<string>();
            var action = doc.RootElement.GetProperty("action").GetString()!;
            var id = doc.RootElement.GetProperty("id").GetString()!;
            var resultJson = resultJsonByAction.TryGetValue(action, out var r) ? r : "{\"kind\":\"error\",\"message\":\"unmapped\"}";
            return new[] { "{\"id\":\"" + id + "\",\"ok\":true,\"result\":" + resultJson + "}" };
        };
    }

    private static readonly string TwoActionHandshake =
        "{\"event\":\"handshake\",\"protocolVersion\":1,\"engineVersion\":\"0.1.0\",\"generation\":1," +
        "\"actions\":[" +
        "{\"id\":\"ohg.program.cut\",\"title\":\"Cut\",\"description\":\"Cut to preview\",\"params\":[]}," +
        "{\"id\":\"ohg.look.set\",\"title\":\"Set look\",\"description\":\"Select a look\"," +
        "\"params\":[{\"name\":\"pin\",\"type\":\"string\",\"required\":true,\"description\":\"PIN\"}," +
        "{\"name\":\"live\",\"type\":\"bool\",\"required\":false,\"description\":\"Go live\"}]}" +
        "],\"fieldTemplates\":[\"ohg/look\",\"ohg/slot/*/name\"]," +
        "\"snapshot\":{\"revision\":3,\"capacity\":10},\"fields\":{\"ohg/look\":\"wide\"}}";

    [Fact]
    public async Task Handshake_PublishesActions_AndFieldTemplates_AsControlActions()
    {
        var factory = new FakeChildFactory();
        factory.Configure = child => child.Push(TwoActionHandshake);
        factory.PreloadHandshake = _ => false; // avoid a duplicate handshake from the default preload
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        var raisedCount = 0;
        bridge.ActionsChanged += (_, _) => raisedCount++;

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => bridge.Actions.Count == 2, "the bridge to publish two actions");

        Assert.Equal("ohg.program.cut", bridge.Actions[0].Id);
        Assert.Equal("ohg.look.set", bridge.Actions[1].Id);
        Assert.Equal(ControlParamType.String, bridge.Actions[1].Params[0].Type);
        Assert.Equal("pin", bridge.Actions[1].Params[0].Name);
        Assert.Equal(ControlParamType.Bool, bridge.Actions[1].Params[1].Type);
        Assert.Equal(2, bridge.FeedbackFieldTemplates.Count);
        Assert.Equal(1, raisedCount);
    }

    [Fact]
    public async Task Invoke_MapsEachResultKind()
    {
        var factory = new FakeChildFactory();
        RespondToInvoke(factory, new Dictionary<string, string>
        {
            ["ohg.program.cut"] = "{\"kind\":\"ok\"}",
            ["ohg.look.nextGuest"] = "{\"kind\":\"refused\",\"reason\":\"manual box fill\"}",
            ["ohg.x"] = "{\"kind\":\"error\",\"message\":\"unknown action\"}",
        });
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running, "the engine to be running");

        var ok = await bridge.InvokeAsync("ohg.program.cut", Array.Empty<object?>(), CancellationToken.None);
        var refused = await bridge.InvokeAsync("ohg.look.nextGuest", Array.Empty<object?>(), CancellationToken.None);
        var error = await bridge.InvokeAsync("ohg.x", Array.Empty<object?>(), CancellationToken.None);

        Assert.True(ok.Ok);
        Assert.False(refused.Ok);
        Assert.Equal("manual box fill", refused.Error);
        Assert.False(error.Ok);
        Assert.Equal("unknown action", error.Error);
    }

    [Fact]
    public async Task Invoke_WhenNotRunning_FailsWithTheHealthState_WithoutSending()
    {
        // "Without sending" can only be proven against a child that EXISTED and could have received
        // an "invoke" line — a supervisor that was never started has no child to inspect at all. So
        // this starts the engine (to get a real Child(1) with a real Written log), stops it (Health
        // reverts to Stopped), and then asserts both the failure message AND that no "invoke" line
        // ever reached that child.
        var factory = new FakeChildFactory();
        factory.Configure = child => child.Push(TwoActionHandshake);
        factory.PreloadHandshake = _ => false;
        var delays = new DelayController();
        delays.SetImmediate(Options.RequestTimeout, Options.StopGrace);
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running, "gen 1 running");

        await sup.StopAsync();
        await WaitUntil(() => sup.Health.State == ShowEngineState.Stopped, "gen 1 to stop");

        var result = await bridge.InvokeAsync("ohg.program.cut", Array.Empty<object?>(), CancellationToken.None);

        Assert.False(result.Ok);
        Assert.Equal("OHG show engine is stopped", result.Error);
        Assert.DoesNotContain(factory.Child(1).Written, l => FakeShowEngineChild.TypeOf(l) == "invoke");
    }

    [Fact]
    public async Task Rearm_AfterRespawn_SendsCapacityThenRosterThenSpeaker()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        delays.SetImmediate(ShowEngineRestartPolicy.Delays[0]);   // fire the gen-2 respawn backoff at once
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running, "gen 1 running");

        bridge.PublishCapacity(10);
        bridge.PublishRoster(new[]
        {
            new ShowEngineParticipant("p1", "Alice", true, true, true, false, 0),
        });
        bridge.PublishActiveSpeaker("p1");

        await WaitUntil(() => factory.Child(1).Written.Count(l => FakeShowEngineChild.TypeOf(l) == "activeSpeaker") == 1,
            "gen 1 to have received the active speaker");

        // Kill generation 1; the supervisor respawns generation 2, which handshakes via the default preload.
        factory.Child(1).Complete(-1);
        await WaitUntil(() => factory.SpawnCount == 2, "generation 2 to spawn");
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running && sup.Health.Generation == 2,
            "gen 2 to become running");

        await WaitUntil(() => factory.Child(2).Written.Count(l => FakeShowEngineChild.TypeOf(l) == "activeSpeaker") == 1,
            "gen 2 to receive the re-armed active speaker");

        var gen2Written = factory.Child(2).Written;
        var gen2Types = gen2Written.Select(FakeShowEngineChild.TypeOf).ToList();
        var capacityIndex = gen2Types.IndexOf("capacity");
        var rosterIndex = gen2Types.IndexOf("zoomEvent");
        var speakerIndex = gen2Types.IndexOf("activeSpeaker");

        Assert.True(capacityIndex >= 0, "capacity was never re-sent to gen 2");
        Assert.True(rosterIndex >= 0, "roster was never re-sent to gen 2");
        Assert.True(speakerIndex >= 0, "active speaker was never re-sent to gen 2");
        Assert.True(capacityIndex < rosterIndex, "capacity must precede roster");
        Assert.True(rosterIndex < speakerIndex, "roster must precede active speaker");

        // The exact wire shape, not just "a line of this type appeared": the capacity value and the
        // full nested zoomEvent/roster/participant payload, field name for field name.
        using (var capacityDoc = JsonDocument.Parse(gen2Written[capacityIndex]))
        {
            Assert.Equal(10, capacityDoc.RootElement.GetProperty("capacity").GetInt32());
        }

        using (var rosterDoc = JsonDocument.Parse(gen2Written[rosterIndex]))
        {
            var evt = rosterDoc.RootElement.GetProperty("event");
            Assert.Equal("roster", evt.GetProperty("kind").GetString());
            var participants = evt.GetProperty("participants");
            Assert.Equal(1, participants.GetArrayLength());
            var p = participants[0];
            Assert.Equal("p1", p.GetProperty("participantId").GetString());
            Assert.Equal("Alice", p.GetProperty("rawName").GetString());
            Assert.True(p.GetProperty("online").GetBoolean());
            Assert.True(p.GetProperty("videoOn").GetBoolean());
            Assert.True(p.GetProperty("audioOn").GetBoolean());
            Assert.False(p.GetProperty("handRaised").GetBoolean());
            Assert.Equal(0, p.GetProperty("zoomRole").GetInt32());
        }
    }

    [Fact]
    public async Task ActiveSpeaker_IsSentOnlyOnChange()
    {
        var factory = new FakeChildFactory();
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => sup.Health.State == ShowEngineState.Running, "gen 1 running");

        bridge.PublishActiveSpeaker("p1");
        bridge.PublishActiveSpeaker("p1");
        bridge.PublishActiveSpeaker(null); // clears, sends nothing
        bridge.PublishActiveSpeaker("p2");

        await WaitUntil(() => factory.Child(1).Written.Count(l => FakeShowEngineChild.TypeOf(l) == "activeSpeaker") == 2,
            "exactly two active-speaker sends");

        var sent = factory.Child(1).Written.Where(l => FakeShowEngineChild.TypeOf(l) == "activeSpeaker").ToList();
        Assert.Equal(2, sent.Count);
        using (var first = JsonDocument.Parse(sent[0]))
            Assert.Equal("p1", first.RootElement.GetProperty("participantId").GetString());
        using (var second = JsonDocument.Parse(sent[1]))
            Assert.Equal("p2", second.RootElement.GetProperty("participantId").GetString());
    }

    [Fact]
    public async Task ManifestDrift_BetweenGenerations_IsLogged_AndTheNewOneWins()
    {
        var callCount = 0;
        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        factory.Configure = child => child.Push(
            Interlocked.Increment(ref callCount) == 1
                ? TwoActionHandshake
                : "{\"event\":\"handshake\",\"protocolVersion\":1,\"engineVersion\":\"0.1.0\",\"generation\":" + child.Generation + "," +
                  "\"actions\":[{\"id\":\"ohg.program.cut\",\"title\":\"Cut\",\"description\":\"Cut\",\"params\":[]}]," +
                  "\"fieldTemplates\":[],\"snapshot\":{},\"fields\":{}}");

        var delays = new DelayController();
        delays.SetImmediate(ShowEngineRestartPolicy.Delays[0]);   // fire the gen-2 respawn backoff at once
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        var warnings = new List<string>();
        bridge.Log += (_, l) => { if (l.Level == "warn") lock (warnings) warnings.Add(l.Message); };

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => bridge.Actions.Count == 2, "gen 1 to publish two actions");

        factory.Child(1).Complete(-1);
        await WaitUntil(() => factory.SpawnCount == 2, "gen 2 to spawn");
        await WaitUntil(() => bridge.Actions.Count == 1, "gen 2's smaller manifest to win");

        Assert.Equal("ohg.program.cut", bridge.Actions[0].Id);
        await WaitUntil(() => warnings.Any(w => w.Contains("engine manifest changed between generations")),
            "a manifest-drift warning");
        lock (warnings) Assert.Contains(warnings, w => w.Contains("2") && w.Contains("1") && w.Contains("ohg.look.set"));
    }

    [Fact]
    public async Task StoppedOrFailed_ClearsActions()
    {
        var factory = new FakeChildFactory();
        factory.Configure = child => child.Push(TwoActionHandshake);
        factory.PreloadHandshake = _ => false;
        var delays = new DelayController();
        // The fake child never answers "shutdown", so StopAsync's own request must time out (rather
        // than park forever waiting on a reply that will never come), and the post-shutdown grace
        // wait must also fire at once so StopAsync falls through to Kill() and returns.
        delays.SetImmediate(Options.RequestTimeout, Options.StopGrace);
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        var raises = 0;
        bridge.ActionsChanged += (_, _) => raises++;

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => bridge.Actions.Count == 2, "actions to publish");
        Assert.Equal(1, raises);

        await sup.StopAsync();
        await WaitUntil(() => bridge.Actions.Count == 0, "actions to clear on Stopped");
        Assert.Empty(bridge.FeedbackFieldTemplates);
        Assert.True(raises >= 2);
    }

    [Fact]
    public async Task StoppedOrFailed_ClearsActions_OnFailedToo()
    {
        // A separate scenario from the Stopped one above: a crash with NO retry budget left goes
        // straight from Running to Failed (never through Stopped), so the clear-on-Failed branch is
        // the only thing that can make this test pass.
        var factory = new FakeChildFactory();
        factory.Configure = child => child.Push(TwoActionHandshake);
        factory.PreloadHandshake = _ => false;
        var delays = new DelayController();
        var noRetryPolicy = new ShowEngineRestartPolicy(maxConsecutiveFailures: 0);
        using var sup = new ShowEngineSupervisor(factory, noRetryPolicy, delays.DelayAsync, () => Clock, Options);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => bridge.Actions.Count == 2, "actions to publish");

        factory.Child(1).Complete(-1);   // a crash with zero retry budget -> straight to Failed
        await WaitUntil(() => sup.Health.State == ShowEngineState.Failed, "the engine to fail terminally");
        await WaitUntil(() => bridge.Actions.Count == 0, "actions to clear on Failed");
        Assert.Empty(bridge.FeedbackFieldTemplates);
    }

    [Fact]
    public async Task BadManifest_ThroughARealHandshake_LeavesActionsEmpty_AndLogsAnError()
    {
        // Drives an actually-broken manifest (an unknown param type) through a REAL supervisor
        // handshake — not a direct call to ToControlActions — so this proves the bridge's own
        // catch/clear/log/ActionsChanged wiring in OnHandshaken, not just the pure mapper.
        const string badHandshake =
            "{\"event\":\"handshake\",\"protocolVersion\":1,\"engineVersion\":\"0.1.0\",\"generation\":1," +
            "\"actions\":[{\"id\":\"ohg.bad\",\"title\":\"Bad\",\"description\":\"desc\"," +
            "\"params\":[{\"name\":\"weird\",\"type\":\"vector3\",\"required\":true,\"description\":\"nope\"}]}]," +
            "\"fieldTemplates\":[\"ohg/look\"],\"snapshot\":{},\"fields\":{}}";

        var factory = new FakeChildFactory { PreloadHandshake = _ => false };
        factory.Configure = child => child.Push(badHandshake);
        var delays = new DelayController();
        using var sup = NewSupervisor(factory, delays);
        using var bridge = new ShowEngineBridge(sup, OscExposure.LoopbackOnly);

        var errors = new List<string>();
        bridge.Log += (_, l) => { if (l.Level == "error") lock (errors) errors.Add(l.Message); };
        var raisedCount = 0;
        bridge.ActionsChanged += (_, _) => raisedCount++;

        await sup.StartAsync(Request, CancellationToken.None);
        await WaitUntil(() => { lock (errors) return errors.Count > 0; }, "the bad-manifest error log");

        Assert.Empty(bridge.Actions);
        Assert.Empty(bridge.FeedbackFieldTemplates);
        Assert.Equal(1, raisedCount);
        lock (errors) Assert.Contains(errors, e => e.Contains("unknown param type 'vector3' on ohg.bad"));
    }

    [Fact]
    public void ToControlActions_RejectsUnknownParamTypes()
    {
        var defs = new List<ShowEngineActionDefinition>
        {
            new("ohg.bad", "Bad", "desc", new List<ShowEngineActionParam>
            {
                new("weird", "vector3", true, "an unsupported type"),
            })
        };

        var ex = Assert.Throws<InvalidOperationException>(() => ShowEngineBridge.ToControlActions(defs));
        Assert.Contains("unknown param type 'vector3' on ohg.bad", ex.Message);
    }
}
