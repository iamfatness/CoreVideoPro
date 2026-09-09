using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;
namespace CoreVideoPro.MediaCore.Tests;

public sealed class AtomicTakeClientTests
{
    private static NativeTakeRequest Request() => new("show", "stable-operation",
        new(10, 8, "media", 7, new("cut", 0),
            new(new("show", 10, 7), "plan", 11, 1, new("show", "registry", 10, 12, "eligibility"))));
    private const string Applied = """
        {"ok":true,"type":"take-result","replayed":false,"retryable":false,"reconcileRequired":false,
        "outcome":{"authorityEpoch":"show","operationId":"stable-operation","error":"none","pending":false,
        "accepted":true,"applied":true,"rendered":false,"delivered":false,"resultRevision":11,"failure":""}}
        """;
    [Theory]
    [InlineData(false, 0)]
    [InlineData(false, 1)]
    [InlineData(true, 0)]
    [InlineData(true, 2)]
    public async Task BothLocalOptInAndExactPeerCapabilityAreRequired(bool enabled, int version)
    {
        var calls = 0;
        var client = new AtomicTakeClient((_, _) => { calls++; return Task.FromResult(Applied); }, new(enabled, version));
        Assert.Equal("capability-disabled", (await client.TakeAsync(Request())).Error);
        Assert.Equal(0, calls);
    }
    [Fact]
    public async Task DefaultIsDisabledAndDoesNotSend()
    {
        var client = new AtomicTakeClient((_, _) => throw new Exception("must not send"));
        Assert.Equal("capability-disabled", (await client.TakeAsync(Request())).Error);
    }
    [Fact]
    public async Task RequestRetainsCallerIdentityAndAppliedDoesNotMeanRendered()
    {
        string? sent = null;
        var client = new AtomicTakeClient((json, _) => { sent = json; return Task.FromResult(Applied); }, new(true, 1, true));
        var request = Request();
        var reply = await client.TakeAsync(request);
        Assert.Equal(request.Serialize(), sent);
        Assert.Equal("none", reply.Error);
        Assert.True(reply.Outcome!.Applied);
        Assert.False(reply.Outcome.Rendered);
        Assert.False(reply.Outcome.Delivered);
    }
    [Fact]
    public void DeliveredAndReplayArePreservedOnlyWhenExplicit()
    {
        var node = JsonNode.Parse(Applied)!;
        node["replayed"] = true;
        node["outcome"]!["rendered"] = true;
        node["outcome"]!["delivered"] = true;
        var reply = AtomicTakeReplyCodec.Decode(node.ToJsonString(), Request());
        Assert.True(reply.Replayed);
        Assert.True(reply.Outcome!.Delivered);
    }
    [Theory]
    [InlineData("operationId", "\"other-operation\"")]
    [InlineData("authorityEpoch", "\"retired-show\"")]
    [InlineData("resultRevision", "12")]
    [InlineData("resultRevision", "11.5")]
    [InlineData("resultRevision", "9007199254740992")]
    [InlineData("error", "\"future-error\"")]
    [InlineData("delivered", "true")]
    [InlineData("pending", "true")]
    public void InvalidOrUncorrelatedEvidenceRequiresReconciliation(string field, string json)
    {
        var node = JsonNode.Parse(Applied)!;
        node["outcome"]![field] = JsonNode.Parse(json);
        var reply = AtomicTakeReplyCodec.Decode(node.ToJsonString(), Request());
        Assert.Equal("invalid-response", reply.Error);
        Assert.Null(reply.Outcome);
        Assert.True(reply.ReconcileRequired);
    }
    [Fact]
    public void MissingAndDuplicateEvidenceCannotDefaultToSuccess()
    {
        var node = JsonNode.Parse(Applied)!;
        node["outcome"]!.AsObject().Remove("accepted");
        Assert.Null(AtomicTakeReplyCodec.Decode(node.ToJsonString(), Request()).Outcome);
        Assert.Null(AtomicTakeReplyCodec.Decode(Applied.Replace("\"ok\":true", "\"ok\":false,\"ok\":true"), Request()).Outcome);
    }
    [Fact]
    public async Task TransportFailureIsUnknownAndNeverAutomaticallyRetried()
    {
        var calls = 0;
        var client = new AtomicTakeClient((_, _) => { calls++; throw new IOException("lost response"); }, new(true, 1, true));
        var reply = await client.TakeAsync(Request());
        Assert.Equal(1, calls);
        Assert.Equal("transport-unknown", reply.Error);
        Assert.True(reply.ReconcileRequired);
        Assert.Null(reply.Outcome);
    }
    [Theory]
    [InlineData("capability-disabled", false)]
    [InlineData("invalid-request", false)]
    [InlineData("outcome-unavailable", true)]
    public void NativeBoundaryErrorsPreserveReconciliationRequirement(string error, bool reconcile)
    {
        var json = JsonSerializer.Serialize(new { ok = false, type = "take-result", error, reconcileRequired = reconcile });
        var reply = AtomicTakeReplyCodec.Decode(json, Request());
        Assert.Equal(error, reply.Error); Assert.Equal(reconcile, reply.ReconcileRequired);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DisabledNativeCapabilityDoesNotEnableFromVersionAlone(bool explicitlyFalse)
    {
        using var document = JsonDocument.Parse("{\"atomicTake\":false,\"atomicTakeVersion\":1,\"clientMediaObservations\":false}");
        var root = document.RootElement;
        var capability = explicitlyFalse
            ? new AtomicTakeClientCapability(true, root.GetProperty("atomicTakeVersion").GetInt32(), root.GetProperty("atomicTake").GetBoolean())
            : new AtomicTakeClientCapability(true, root.GetProperty("atomicTakeVersion").GetInt32());
        var client = new AtomicTakeClient((_, _) => throw new Exception("must not dispatch"), capability);
        Assert.Equal("capability-disabled", (await client.TakeAsync(Request())).Error);
    }
    [Fact]
    public async Task PostDispatchDisposalIsUnknownAndPreCancelledNeverDispatches()
    {
        var calls = 0;
        var client = new AtomicTakeClient((_, _) => { calls++; throw new ObjectDisposedException("transport"); }, new(true, 1, true));
        var reply = await client.TakeAsync(Request());
        Assert.Equal("transport-unknown", reply.Error);
        Assert.True(reply.ReconcileRequired);
        Assert.Equal(1, calls);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => client.TakeAsync(Request(), new CancellationToken(true)));
        Assert.Equal(1, calls);
    }
    [Theory]
    [InlineData(false, false, false, "none", 11)]
    [InlineData(true, false, false, "none", 11)]
    [InlineData(true, true, false, "applyFailed", 10)]
    [InlineData(true, true, false, "none", 10)]
    [InlineData(true, false, false, "applyFailed", 11)]
    public void ImpossibleSuccessPendingAndRevisionShapesFailClosed(bool accepted, bool pending, bool applied, string error, long revision)
    {
        var node = JsonNode.Parse(Applied)!;
        node["ok"] = error == "none";
        node["reconcileRequired"] = pending;
        var outcome = node["outcome"]!;
        outcome["accepted"] = accepted; outcome["pending"] = pending; outcome["applied"] = applied;
        outcome["error"] = error; outcome["resultRevision"] = revision;
        var reply = AtomicTakeReplyCodec.Decode(node.ToJsonString(), Request());
        Assert.Equal("invalid-response", reply.Error); Assert.True(reply.ReconcileRequired); Assert.Null(reply.Outcome);
    }
    [Fact]
    public void ValidPendingAndApplyFailureRetainDistinctRevisionMeaning()
    {
        var node = JsonNode.Parse(Applied)!;
        node["reconcileRequired"] = true; node["outcome"]!["pending"] = true; node["outcome"]!["applied"] = false;
        Assert.True(AtomicTakeReplyCodec.Decode(node.ToJsonString(), Request()).Outcome!.Pending);
        node["reconcileRequired"] = false; node["ok"] = false;
        node["outcome"]!["pending"] = false; node["outcome"]!["error"] = "applyFailed"; node["outcome"]!["resultRevision"] = 10;
        var reply = AtomicTakeReplyCodec.Decode(node.ToJsonString(), Request());
        Assert.Equal("applyFailed", reply.Error); Assert.True(reply.Outcome!.Accepted); Assert.False(reply.Outcome.Applied);
    }
}
