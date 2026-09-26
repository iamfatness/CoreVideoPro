using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class AudioMonitorControlCoordinatorTests
{
    private static NativeMediaCoreStateSnapshot Snapshot(long revision, bool enabled,
        NativeMediaCoreMonitorControlResult? result = null,
        IReadOnlyList<NativeMediaCoreMonitorControlResult>? results = null) => new()
    {
        AudioMixSession = new NativeMediaCoreAudioMixSession
        {
            Status = "armed", Summary = "Monitor", MonitorEnabled = enabled,
            MonitorVolume = 0.5,
            MonitorControl = new NativeMediaCoreMonitorControl
            {
                AuthorityEpoch = "core-1", Revision = revision,
                LastResult = result, RecentResults = results ?? (result is null ? [] : [result])
            }
        }
    };

    private static readonly MediaCoreAudioMonitorWire Draft = new(true, "", "", 0.5);

    [Fact]
    public async Task TwoClientsCannotOverwriteStaleRevision_AndRetryIsIdempotent()
    {
        var native = Snapshot(0, false);
        var mutations = 0;
        var results = new List<NativeMediaCoreMonitorControlResult>();
        async Task<NativeMediaCoreStateSnapshot> Send(IReadOnlyList<NativeMediaCoreCommand> commands)
        {
            var data = commands.Single().ExtensionData!;
            Assert.Equal("set-audio-monitor-control", commands.Single().Type);
            var operationId = data["operationId"].GetString()!;
            var expected = data["expectedRevision"].GetInt64();
            var control = native.AudioMixSession.MonitorControl!;
            var prior = results.FirstOrDefault(r => r.OperationId == operationId);
            var status = prior?.Status ?? (expected == control.Revision ? "applied" : "conflict");
            if (prior is null && status == "applied") mutations++;
            var outcome = prior ?? new NativeMediaCoreMonitorControlResult
                {
                    OperationId = operationId, Status = status,
                    AuthorityEpoch = "core-1", ExpectedRevision = expected,
                    Revision = prior?.Revision ?? (status == "applied" ? control.Revision + 1 : control.Revision)
                };
            if (prior is null) results.Add(outcome);
            native = Snapshot(status == "applied" && prior is null ? control.Revision + 1 : control.Revision,
                status == "applied" && prior is null ? data["enabled"].GetBoolean() : native.AudioMixSession.MonitorEnabled,
                outcome, results);
            return await Task.FromResult(native);
        }
        var a = new AudioMonitorControlCoordinator(Send, () => Task.FromResult(native));
        var b = new AudioMonitorControlCoordinator(Send, () => Task.FromResult(native));
        a.Observe(native);
        b.Observe(native);
        var first = await a.SubmitAsync(Draft, operationId: "client-a-1");
        Assert.Equal(AudioMonitorControlOutcomeKind.Applied, first.Kind);
        Assert.Equal(1, mutations);
        var conflict = await b.SubmitAsync(Draft with { Enabled = false }, operationId: "client-b-1");
        Assert.Equal(AudioMonitorControlOutcomeKind.Conflict, conflict.Kind);
        Assert.Equal(1, mutations);
        var retry = await a.SubmitAsync(Draft, expectedRevision: 0, operationId: "client-a-1");
        Assert.Equal(AudioMonitorControlOutcomeKind.Applied, retry.Kind);
        Assert.Equal(1, mutations);
    }

    [Fact]
    public async Task LostReplyReconcilesBySnapshotWithoutResending()
    {
        var native = Snapshot(0, false);
        var sends = 0;
        var coordinator = new AudioMonitorControlCoordinator(commands =>
        {
            sends++;
            var operationId = commands.Single().ExtensionData!["operationId"].GetString()!;
            native = Snapshot(1, true, new NativeMediaCoreMonitorControlResult
            {
                OperationId = operationId, Status = "applied", AuthorityEpoch = "core-1",
                ExpectedRevision = 0, Revision = 1
            });
            throw new IOException("reply lost after native apply");
        }, () => Task.FromResult(native));
        var outcome = await coordinator.SubmitAsync(Draft, operationId: "lost-reply");
        Assert.Equal(AudioMonitorControlOutcomeKind.Applied, outcome.Kind);
        Assert.Equal(1, sends);
    }
}
