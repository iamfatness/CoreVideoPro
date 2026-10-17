using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsCommandGatewayTests
{
    private static NativeGraphicsState Observed => new()
    {
        SchemaVersion = 1, CoreEpoch = "test-core",
        Instances = [new() { InstanceId = "featured", Revision = 1, Fields = new() { Name = "Guest" } }]
    };

    private static string Request(string extra = "", string id = "request-one") =>
        "{\"schemaVersion\":1,\"requestId\":\"" + id + "\",\"clientId\":\"operator\",\"coreEpoch\":\"test-core\"," +
        "\"instanceId\":\"featured\",\"expectedRevision\":1" + (extra.Length == 0 ? "" : "," + extra) + "}";

    private static NativeMediaCoreStateSnapshot Ack(NativeMediaCoreCommand command, bool accepted = true,
        string reason = "cue-accepted", string? id = null) => new()
    {
        Graphics = Observed with
        {
            LastResult = new()
            {
                RequestId = id ?? command.ExtensionData!["graphics"].GetProperty("requestId").GetString()!,
                Accepted = accepted, Reason = reason
            }
        }
    };

    [Fact]
    public async Task RealCommandShapePreservesExplicitBlankAndNativeAdmission()
    {
        NativeMediaCoreCommand? sent = null;
        var gateway = new GraphicsCommandGateway(() => true, () => new() { Graphics = Observed }, (commands, _) =>
        {
            sent = Assert.Single(commands);
            return Task.FromResult(Ack(sent));
        });
        var result = await gateway.InvokeAsync("graphics.instance.update", Request("\"target\":\"cue\",\"fields\":{\"title\":\"\"}"));
        Assert.True(result.Accepted);
        Assert.Equal("graphics-instance-update", sent!.Type);
        Assert.Equal("", sent.ExtensionData!["graphics"].GetProperty("fields").GetProperty("title").GetString());
        Assert.Equal(1, sent.ExtensionData["graphics"].GetProperty("expectedRevision").GetInt32());
        Assert.Equal("cue", sent.ExtensionData["graphics"].GetProperty("target").GetString());
    }

    [Theory]
    [InlineData("graphics.instance.trigger", "\"action\":\"show\",\"target\":\"cue\"")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"target\":\"program\"")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"fields\":{\"name\":\"One\",\"name\":\"Two\"}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"style\":{\"nameSize\":\"huge\"}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"style\":{\"unknownSlider\":50}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"bindings\":{}")]
    [InlineData("graphics.program.clear", "\"channel\":\"unknown\"")]
    public async Task InvalidOrIgnoredLookingControlsNeverReachTheCore(string action, string extra)
    {
        var sends = 0;
        var gateway = new GraphicsCommandGateway(() => true, () => new() { Graphics = Observed }, (commands, _) =>
        {
            sends++;
            return Task.FromResult(Ack(commands[0]));
        });
        Assert.False((await gateway.InvokeAsync(action, Request(extra))).Accepted);
        Assert.Equal(0, sends);
    }

    [Fact]
    public void IntegerDecimalNotationAndPartialStyleAreAcceptedWithoutRewritingThePayload()
    {
        var request = Request("\"target\":\"cue\",\"style\":{\"version\":1.0,\"nameSize\":65}")
            .Replace("\"expectedRevision\":1", "\"expectedRevision\":1.0");
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.update", request, Observed, out var command, out _, out _));
        Assert.Equal("1.0", command!.ExtensionData!["graphics"].GetProperty("expectedRevision").GetRawText());
        Assert.Equal(65, command.ExtensionData["graphics"].GetProperty("style").GetProperty("nameSize").GetInt32());
    }

    [Fact]
    public void OversizedFieldsAndStaleEpochAreRefused()
    {
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.instance.update",
            Request("\"target\":\"cue\",\"fields\":{\"name\":\"" + new string('x', 257) + "\"}"),
            Observed, out _, out _, out _));
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.instance.cue", Request().Replace("test-core", "old-core"),
            Observed, out _, out _, out var error));
        Assert.Equal("core-epoch-conflict", error);
    }

    [Fact]
    public async Task NativeRefusalAndUncorrelatedResponsesCannotBecomeSuccess()
    {
        var nativeRefusal = new GraphicsCommandGateway(() => true, () => new() { Graphics = Observed },
            (commands, _) => Task.FromResult(Ack(commands[0], false, "revision-conflict")));
        var result = await nativeRefusal.InvokeAsync("graphics.instance.cue", Request());
        Assert.False(result.Accepted);
        Assert.Equal("revision-conflict", result.Reason);
        var mismatched = new GraphicsCommandGateway(() => true, () => new() { Graphics = Observed },
            (commands, _) => Task.FromResult(Ack(commands[0], id: "another-request")));
        Assert.Equal("graphics-result-not-correlated", (await mismatched.InvokeAsync("graphics.instance.cue", Request())).Reason);
    }

    [Fact]
    public async Task BusyGatewayRefusesNewIntentAndRecoversAfterTheFirstAdmission()
    {
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var sends = 0;
        var gateway = new GraphicsCommandGateway(() => true, () => new() { Graphics = Observed }, async (commands, _) =>
        {
            Interlocked.Increment(ref sends);
            await release.Task;
            return Ack(commands[0]);
        });
        var first = gateway.InvokeAsync("graphics.instance.cue", Request());
        var second = await gateway.InvokeAsync("graphics.instance.cue", Request(id: "request-two"));
        Assert.False(second.Accepted);
        Assert.Equal("graphics-command-overloaded", second.Reason);
        Assert.Equal(1, sends);
        release.SetResult();
        Assert.True((await first).Accepted);
        Assert.True((await gateway.InvokeAsync("graphics.instance.cue", Request(id: "request-three"))).Accepted);
        Assert.Equal(2, sends);
    }

    [Fact]
    public async Task StoppedOrOldCoreIsNotLaunchedByAnInvocation()
    {
        var sends = 0;
        var gateway = new GraphicsCommandGateway(() => false, () => new() { Graphics = Observed }, (commands, _) =>
        {
            sends++;
            return Task.FromResult(Ack(commands[0]));
        });
        Assert.False((await gateway.InvokeAsync("graphics.instance.cue", Request())).Accepted);
        var old = new GraphicsCommandGateway(() => true, () => new(), (commands, _) =>
        {
            sends++;
            return Task.FromResult(Ack(commands[0]));
        });
        Assert.False((await old.InvokeAsync("graphics.instance.cue", Request())).Accepted);
        Assert.Equal(0, sends);
    }
}
