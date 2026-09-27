using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class AudioRouteControlCoordinatorTests
{
    private static readonly MediaCoreAudioRouteWire Route = new("zoom:42", "mon", true, -6);

    private static NativeMediaCoreStateSnapshot Snapshot(string epoch, long revision,
        bool routed = false, NativeMediaCoreAudioRouteResult? result = null,
        IReadOnlyList<NativeMediaCoreAudioRouteResult>? results = null) => new()
    {
        AudioRoutingMatrix = new NativeMediaCoreAudioRoutingMatrix
        {
            Status = routed ? "live" : "idle", Summary = "Audio route",
            Sends = routed ? [new NativeMediaCoreAudioRoutingSend
                { SourceId = "zoom:42", BusId = "mon", GainDb = -6 }] : [],
            Control = new NativeMediaCoreAudioRouteControl
            {
                AuthorityEpoch = epoch, Revision = revision, LastResult = result,
                RecentResults = results ?? (result is null ? [] : [result])
            }
        }
    };

    [Fact]
    public async Task TwoClientsConflict_DuplicateRetryDoesNotApplyAgain()
    {
        var native = Snapshot("core-1", 0);
        var mutations = 0;
        var results = new List<NativeMediaCoreAudioRouteResult>();
        Task<NativeMediaCoreStateSnapshot> Send(IReadOnlyList<NativeMediaCoreCommand> commands)
        {
            var command = commands.Single();
            Assert.Equal("set-audio-route-control", command.Type);
            var data = command.ExtensionData!;
            Assert.Equal("zoom:42", data["sourceId"].GetString());
            Assert.Equal("mon", data["busId"].GetString());
            var id = data["operationId"].GetString()!;
            var expected = data["expectedRevision"].GetInt64();
            var control = native.AudioRoutingMatrix.Control!;
            var prior = results.FirstOrDefault(item => item.OperationId == id);
            var status = prior?.Status ?? (expected == control.Revision ? "applied" : "conflict");
            if (prior is null && status == "applied") mutations++;
            var result = prior ?? new NativeMediaCoreAudioRouteResult
            {
                OperationId = id, Status = status, AuthorityEpoch = "core-1",
                ExpectedRevision = expected,
                Revision = status == "applied" ? control.Revision + 1 : control.Revision
            };
            if (prior is null) results.Add(result);
            var routed = prior is null && status == "applied"
                ? data["enabled"].GetBoolean() : native.AudioRoutingMatrix.Sends.Count > 0;
            native = Snapshot("core-1", prior is null ? result.Revision : control.Revision,
                routed,
                result, results);
            return Task.FromResult(native);
        }
        var a = new AudioRouteControlCoordinator(Send, () => Task.FromResult(native));
        var b = new AudioRouteControlCoordinator(Send, () => Task.FromResult(native));
        a.Observe(native);
        b.Observe(native);
        Assert.Equal(AudioRouteControlOutcomeKind.Applied,
            (await a.SubmitAsync(Route, operationId: "a-1")).Kind);
        var conflict = await b.SubmitAsync(Route with { Enabled = false }, operationId: "b-1");
        Assert.Equal(AudioRouteControlOutcomeKind.Conflict, conflict.Kind);
        Assert.Single(conflict.Applied!.Sends);
        Assert.Equal(AudioRouteControlOutcomeKind.Applied,
            (await a.SubmitAsync(Route, expectedRevision: 0, operationId: "a-1")).Kind);
        Assert.Equal(1, mutations);
    }

    [Fact]
    public async Task LostReplyUsesSnapshotBarrier_AndOldEpochConflictsAfterRestart()
    {
        var native = Snapshot("core-1", 0);
        var sends = 0;
        var coordinator = new AudioRouteControlCoordinator(commands =>
        {
            sends++;
            var data = commands.Single().ExtensionData!;
            var id = data["operationId"].GetString()!;
            if (native.AudioRoutingMatrix.Control!.AuthorityEpoch != data["authorityEpoch"].GetString())
            {
                native = Snapshot("core-2", 0, result: new NativeMediaCoreAudioRouteResult
                {
                    OperationId = id, Status = "conflict", AuthorityEpoch = "core-2",
                    ExpectedRevision = data["expectedRevision"].GetInt64(), Revision = 0
                });
                return Task.FromResult(native);
            }
            native = Snapshot("core-1", 1, routed: true, new NativeMediaCoreAudioRouteResult
            {
                OperationId = id, Status = "applied", AuthorityEpoch = "core-1",
                ExpectedRevision = 0, Revision = 1
            });
            throw new IOException("reply lost after apply");
        }, () => Task.FromResult(native));
        var applied = await coordinator.SubmitAsync(Route, operationId: "lost");
        Assert.Equal(AudioRouteControlOutcomeKind.Applied, applied.Kind);
        Assert.Equal(1, sends);

        native = Snapshot("core-2", 0);
        coordinator.Reset();
        coordinator.Observe(native);
        var stale = await coordinator.SubmitAsync(Route, expectedEpoch: "core-1",
            expectedRevision: 1, operationId: "retired");
        Assert.Equal(AudioRouteControlOutcomeKind.Conflict, stale.Kind);
        Assert.Empty(stale.Applied!.Sends);
    }

    [Fact]
    public void DelayedSnapshotCannotRollBackAppliedRoute()
    {
        var applied = Snapshot("core-1", 2, routed: true);
        var stale = Snapshot("core-1", 1);
        var merged = ControlAudioRouteSnapshotMerger.CarryNewer(applied, stale);
        Assert.Equal(2, merged.AudioRoutingMatrix.Control!.Revision);
        Assert.Single(merged.AudioRoutingMatrix.Sends);
    }
}
