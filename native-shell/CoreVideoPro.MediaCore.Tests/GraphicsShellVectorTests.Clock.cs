using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>G4-G7 Task 4: the countdown transport. Transcripts are recorded from the REAL
/// GraphicsPlayoutService.ClockAsync against a small scripted core that honours clock actions
/// (plan rule 9): each step is the wire command the gateway builds (GraphicsCommandRequest.TryBuild,
/// minus requestId/clientId/schemaVersion/coreEpoch) and the graphics node after it. The readout
/// rows restate W/Controls/GraphicsPlayoutControl.cs:458 (a WinUI expression the exporter cannot
/// call), including C#'s m\:ss TimeSpan format, which drops hours (plan D3).</summary>
public sealed partial class GraphicsShellVectorTests
{
    private static NativeGraphicsInstance CountdownInstance(long revision, string? clockState = null) => new()
    {
        InstanceId = GraphicsPlayoutService.InstanceIdFor("countdown", ""), Revision = revision, ProgramRevision = revision,
        TemplateId = "clock-panel", TemplateVersion = 1, Scope = "global", Visibility = revision > 0 ? "visible" : "hidden",
        Clock = clockState is null ? null : new NativeGraphicsClock { Text = "10:00", State = clockState, FinishAction = "hide", RemainingSeconds = 600,
            Configuration = new NativeGraphicsClockConfiguration { Mode = "countdown", Zone = "utc", Format = "hm", Finish = "hide", DurationSeconds = 600 } },
    };

    /// <summary>Clock state after an action, as the core reports it: start/resume run, pause pauses, reset rearms at the full duration.</summary>
    private static NativeGraphicsClock ClockAfter(NativeGraphicsClock clock, string action) => action switch
    {
        "start" or "resume" => clock with { State = "running", Text = "09:59", RemainingSeconds = 599 },
        "pause" => clock with { State = "paused" },
        "reset" => clock with { State = "ready", Text = "10:00", RemainingSeconds = 600 },
        _ => clock,
    };

    private static JsonNode ClockTranscript(NativeGraphicsState initial, string action, string? entrySourceId = null)
    {
        var state = initial;
        var steps = new JsonArray();
        var directory = Path.Combine(Path.GetTempPath(), "cvp-clock-transcript-" + Guid.NewGuid().ToString("N"));
        try
        {
            Task<NativeGraphicsOperationResult> Invoke(string name, string body, CancellationToken cancellation)
            {
                Assert.True(GraphicsCommandRequest.TryBuild(name, body, state, out var command, out _, out var error), error);
                var graphics = JsonNode.Parse(command!.ExtensionData!["graphics"].GetRawText())!.AsObject();
                foreach (var key in new[] { "requestId", "clientId", "schemaVersion", "coreEpoch" }) graphics.Remove(key);
                var input = JsonDocument.Parse(body).RootElement;
                var id = input.GetProperty("instanceId").GetString()!;
                var clockAction = input.GetProperty("action").GetString()!;
                var revision = state.Instances.Single(i => i.InstanceId == id).Revision + 1;
                state = state with { Instances = state.Instances.Select(i => i.InstanceId != id ? i :
                    i with { Revision = revision, Clock = i.Clock is { } c ? ClockAfter(c, clockAction) : i.Clock }).ToArray() };
                steps.Add(new JsonObject { ["command"] = new JsonObject { ["type"] = command.Type, ["graphics"] = graphics },
                    ["nodeAfter"] = JsonSerializer.SerializeToNode(state, Web) });
                return Task.FromResult(new NativeGraphicsOperationResult { Accepted = true, InstanceId = id, CurrentRevision = revision, Reason = "accepted" });
            }
            var session = new GraphicsWorkspaceSession(Invoke, () => state); session.Observe(state);
            var library = new GraphicsWorkspaceLibrary(new GraphicsDocumentStore(Path.Combine(directory, "show.json")), session);
            var service = new GraphicsPlayoutService(library, Invoke, () => state, (_, _) => Task.CompletedTask, () => DateTime.UnixEpoch);
            var entry = new GraphicsPlaylistEntry("countdown", entrySourceId ?? "", "", "");
            var result = service.ClockAsync(entry, action).GetAwaiter().GetResult();
            return new JsonObject { ["steps"] = steps, ["accepted"] = result.Accepted, ["reason"] = result.Reason };
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, true); }
    }

    [Fact]
    public void ClockTranscripts()
    {
        var rows = new JsonArray();
        NativeGraphicsState Node(params NativeGraphicsInstance[] instances) => new() { SchemaVersion = 1, CoreEpoch = "epoch-1", Instances = instances };
        void Add(string id, NativeGraphicsState initial, string action) =>
            rows.Add(Row(id, "clock", new JsonObject { ["document"] = "documents/windows-names-clock.json", ["rowId"] = "countdown",
                ["initialNode"] = JsonSerializer.SerializeToNode(initial, Web), ["intent"] = new JsonObject { ["op"] = "clock", ["action"] = action } },
                ClockTranscript(initial, action)));
        Add("start-without-instance-refused", Node(), "start");
        foreach (var action in new[] { "start", "pause", "resume", "reset" }) Add(action + "-on-program", Node(CountdownInstance(3, action == "resume" ? "paused" : action == "start" ? "ready" : "running")), action);
        Add("start-fresh-instance-revision-zero", Node(CountdownInstance(0, "ready")), "start");
        Add("invalid-action-refused", Node(CountdownInstance(3, "ready")), "stop");
        var (_, clock) = NamesAndClockRows();
        foreach (var seconds in new[] { 600, 59, 3600, 5400, 86399 })
            rows.Add(Row($"readout-not-cued-{seconds}", "readout", new JsonObject { ["durationSeconds"] = seconds, ["clock"] = null },
                new JsonObject { ["text"] = $"{TimeSpan.FromSeconds(seconds):m\\:ss} · not cued" }));
        var live = CountdownInstance(3, "running").Clock! with { Text = "09:41" };
        rows.Add(Row("readout-instance-clock", "readout", new JsonObject { ["durationSeconds"] = clock.Clock!.DurationSeconds,
            ["clock"] = JsonSerializer.SerializeToNode(live, Web) }, new JsonObject { ["text"] = $"{live.Text} · {live.State}" }));
        Check("clock-transcripts", "Countdown transport: GraphicsPlayoutService.ClockAsync transcripts (wire command + node after each step) and the row card readout (m\\:ss drops hours, plan D3).", rows);
    }
}
