using System.Text.Json;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class GraphicsCommandGatewayTests
{
    private sealed class HeldPackageReader : IGraphicsPackageReader
    {
        public TaskCompletionSource<GraphicsPackageInput> Completion { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Task<GraphicsPackageInput> ReadAsync(string archivePath, CancellationToken cancellation = default) => Completion.Task.WaitAsync(cancellation);
    }
    [Fact]
    public async Task PackageExpansionCannotBlockManualClearAndOnlyVerifiedManifestReachesNative()
    {
        var packages = new HeldPackageReader(); var sent = new List<NativeMediaCoreCommand>();
        var gateway = new GraphicsCommandGateway(() => true, () => new() { Graphics = Observed }, (commands, _) => {
            sent.Add(commands[0]); return Task.FromResult(Ack(commands[0]));
        }, packages);
        var imported = gateway.InvokeAsync("graphics.template.install", Request("\"packagePath\":\"C:/package.zip\"", "import-package"));
        Assert.False(imported.IsCompleted);
        Assert.True((await gateway.InvokeAsync("graphics.program.clear", Request("\"channel\":\"all\"", "clear-now"))).Accepted);
        Assert.Single(sent); Assert.Equal("graphics-program-clear", sent[0].Type);
        packages.Completion.SetResult(new("{\"schemaVersion\":1}", "C:/verified-cache"));
        Assert.True((await imported).Accepted);
        var input = sent[1].ExtensionData!["graphics"];
        Assert.False(input.TryGetProperty("packagePath", out _));
        Assert.Equal("C:/verified-cache", input.GetProperty("packageRoot").GetString());
        Assert.Equal("{\"schemaVersion\":1}", input.GetProperty("manifest").GetString());
    }
    [Fact]
    public void DeclarativeTemplateMustBeAdvertisedAndCannotAcceptIgnoredCompatibilitySliders()
    {
        var request = Request("\"templateId\":\"corporate-lower-third\",\"templateVersion\":1,\"scope\":\"global\",\"fields\":{\"name\":\"Guest\",\"title\":\"\"}");
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.instance.create", request, Observed, out _, out _, out _));
        var advertised = Observed with { Templates = [new() { TemplateId = "corporate-lower-third", TemplateVersion = 1, Runtime = "declarative",
            Fields = [new() { Id = "name", Type = "string", MaxLength = 256 }, new() { Id = "title", Type = "string" }] }] };
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.create", request, advertised, out var command, out _, out _));
        Assert.Equal("", command!.ExtensionData!["graphics"].GetProperty("fields").GetProperty("title").GetString());
        var patch = advertised with { Instances = [new() { InstanceId = "featured", TemplateId = "corporate-lower-third", Revision = 1 }] };
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.instance.update", Request("\"target\":\"cue\",\"style\":{\"width\":0.7}"), patch, out _, out _, out _));
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.update", Request("\"target\":\"cue\",\"style\":{\"fontFamily\":\"Segoe UI\",\"safeOffsetX\":0.1}"), patch, out _, out _, out _));
    }

    [Fact]
    public void ImportsAreBoundedOpaqueNativeWorkAndInstalledVersionsUseTypedFields()
    {
        var request = JsonSerializer.Serialize(new { schemaVersion = 1, requestId = "import-one", clientId = "operator",
            coreEpoch = "test-core", manifest = "{\"schemaVersion\":1}" });
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.template.install", request, Observed, out var install, out _, out _));
        Assert.Equal("graphics-template-install", install!.Type);
        Assert.Equal("{\"schemaVersion\":1}", install.ExtensionData!["graphics"].GetProperty("manifest").GetString());
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.template.install", Request(), Observed, out _, out _, out _));
        request = JsonSerializer.Serialize(new { schemaVersion = 1, requestId = "large", clientId = "operator",
            coreEpoch = "test-core", manifest = new string('x', 256 * 1024 + 1) });
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.template.install", request, Observed, out _, out _, out _));
        var advertised = Observed with { Templates = [new() { TemplateId = "interview", TemplateVersion = 2, Runtime = "declarative",
            Fields = [new() { Id = "value", Type = "number", Minimum = 0, Maximum = 100 }, new() { Id = "enabled", Type = "boolean" }] }] };
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.create",
            Request("\"templateId\":\"interview\",\"templateVersion\":2,\"scope\":\"global\",\"fields\":{\"value\":42,\"enabled\":true}"), advertised, out _, out _, out _));
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.instance.create",
            Request("\"templateId\":\"interview\",\"templateVersion\":2,\"scope\":\"global\",\"fields\":{\"value\":101}"), advertised, out _, out _, out _));
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.instance.create",
            Request("\"templateId\":\"interview\",\"templateVersion\":3,\"scope\":\"global\""), advertised, out _, out _, out _));
    }
    [Fact]
    public async Task InterveningClientResultDoesNotLoseCorrelatedResultHistory()
    {
        var gateway=new GraphicsCommandGateway(()=>true,()=>new() {Graphics=Observed},(commands,_)=>Task.FromResult(new NativeMediaCoreStateSnapshot {Graphics=Observed with {
            LastResult=new() {RequestId="other-client",Accepted=true},Operations=[new() {ClientId="operator",RequestId="request-one",Accepted=true,Reason="queued"}]}}));
        var result=await gateway.InvokeAsync("graphics.program.clear",Request("\"channel\":\"all\""));
        Assert.True(result.Accepted);Assert.Equal("queued",result.Reason);
    }
    [Fact]
    public void AutomationRejectsUnqualifiedOverridesAndPreservesExplicitBlankSecondary()
    {
        var identity=new CoreVideoPro.MediaCore.Contracts.SourceInstanceIdentity {SourceId="zoom:7",InstanceId="guest",ProcessEpoch="meeting",Generation=2};
        string Input(object configuration)=>JsonSerializer.Serialize(new {schemaVersion=1,requestId="rules",clientId="operator",coreEpoch="test-core",expectedAutomationRevision=1,configuration},new JsonSerializerOptions(JsonSerializerDefaults.Web));
        var rules=new[] {new GraphicsAutomationRule {Id="intro",InstanceId="featured"}};
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.automation.configure",Input(new {rules,sourceTexts=new Dictionary<string,object>{{"zoom:7",new {name="Old"}}}}),Observed,out _,out _,out _));
        var good=Input(new GraphicsAutomationConfiguration {Rules=rules,SourceTexts=new Dictionary<string,GraphicsAutomationSourceText>{{"zoom:7",new() {Binding=identity,Secondary=""}}}});
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.automation.configure",good,Observed,out var command,out _,out _));
        Assert.Equal("",command!.ExtensionData!["graphics"].GetProperty("configuration").GetProperty("sourceTexts").GetProperty("zoom:7").GetProperty("secondary").GetString());
    }
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
    [InlineData("graphics.instance.trigger", "\"action\":\"show\",\"target\":\"preview\"")]
    [InlineData("graphics.instance.trigger", "\"action\":\"hide\",\"target\":\"cue\"")]
    [InlineData("graphics.instance.trigger", "\"action\":\"clear\",\"target\":\"cue\"")]
    [InlineData("graphics.instance.trigger", "\"action\":\"show\",\"target\":\"\"")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"target\":\"program\"")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"fields\":{\"name\":\"One\",\"name\":\"Two\"}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"style\":{\"nameSize\":\"huge\"}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"style\":{\"unknownSlider\":50}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"bindings\":{}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"sourceId\":\"zoom:44\"")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"binding\":{\"sourceId\":\"zoom:44\",\"instanceId\":\"source-one\",\"processEpoch\":2,\"generation\":1}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"binding\":{\"sourceId\":\"zoom:44\",\"instanceId\":\"source-one\",\"processEpoch\":\"epoch\",\"generation\":9007199254740992}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"binding\":{\"sourceId\":\"zoom:44\",\"instanceId\":\"source-one\",\"processEpoch\":\"epoch\",\"generation\":1,\"generation\":2}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"sourceId\":\"zoom:99\",\"binding\":{\"sourceId\":\"zoom:44\",\"instanceId\":\"source-one\",\"processEpoch\":\"epoch\",\"generation\":1}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"animations\":{\"in\":{\"type\":\"script\"}}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"animations\":{\"in\":{\"durationMs\":3001}}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"animations\":{\"in\":{\"direction\":\"diagonal\"}}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"animations\":{\"in\":{\"type\":\"fade\",\"type\":\"slide\"}}")]
    [InlineData("graphics.instance.update", "\"target\":\"cue\",\"animations\":{\"in\":{},\"in\":{}}")]
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
    public void ShowFromCueKeepsItsExplicitTargetAndRevisionForNativeValidation()
    {
        var request = Request("\"target\":\"cue\",\"action\":\"show\"");
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.trigger", request, Observed, out var command, out _, out _));
        Assert.Equal("cue", command!.ExtensionData!["graphics"].GetProperty("target").GetString());
        Assert.Equal(1, command.ExtensionData["graphics"].GetProperty("expectedRevision").GetInt32());
    }

    [Fact]
    public void ValidNativeTransitionsRoundTripWithoutShellTiming()
    {
        var request = Request("\"target\":\"cue\",\"animations\":{\"in\":{\"type\":\"slide\",\"direction\":\"up\",\"durationMs\":250,\"easing\":\"ease-out\"},\"out\":{\"type\":\"fade\",\"durationMs\":300}}");
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.update", request, Observed, out var command, out _, out _));
        Assert.Equal(250, command!.ExtensionData!["graphics"].GetProperty("animations").GetProperty("in").GetProperty("durationMs").GetInt32());
        Assert.Equal("fade", command.ExtensionData["graphics"].GetProperty("animations").GetProperty("out").GetProperty("type").GetString());
    }

    [Fact]
    public void QualifiedBindingRoundTripsLosslesslyAndResetIsExplicit()
    {
        var request = Request("\"target\":\"cue\",\"binding\":{\"sourceId\":\"zoom:44\",\"instanceId\":\"source-one\",\"processEpoch\":\"native:18446744073709551615\",\"generation\":1.0}");
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.update", request, Observed, out var command, out _, out _));
        var binding = command!.ExtensionData!["graphics"].GetProperty("binding");
        Assert.Equal("native:18446744073709551615", binding.GetProperty("processEpoch").GetString());
        Assert.Equal("1.0", binding.GetProperty("generation").GetRawText());
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.update", Request("\"target\":\"cue\",\"binding\":null"), Observed, out command, out _, out _));
        Assert.Equal(JsonValueKind.Null, command!.ExtensionData!["graphics"].GetProperty("binding").ValueKind);
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
    [Fact]
    public void ClockConfigurationRequiresAllTypedFieldsAndRejectsUnknownOrUnsafeValues()
    {
        var config=new NativeGraphicsClockConfiguration { Mode="countdown",DurationSeconds=90,RestoreRemainingSeconds=12.5 };
        var options=new JsonSerializerOptions(JsonSerializerDefaults.Web);
        string Input(NativeGraphicsClockConfiguration value)=>Request("\"action\":\"configure\",\"expectedClockRevision\":1,\"configuration\":"+JsonSerializer.Serialize(value,options));
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.clock.control",Input(config),Observed,out var command,out _,out _));
        Assert.Equal("graphics-clock-control",command!.Type);
        foreach(var invalid in new[] { config with { DurationSeconds=0 },config with { OffsetMinutes=841 },config with { RestoreRemainingSeconds=100 },config with { Mode="script" } })
            Assert.False(GraphicsCommandRequest.TryBuild("graphics.clock.control",Input(invalid),Observed,out _,out _,out _));
        var missing=JsonSerializer.SerializeToNode(config,options)!.AsObject(); missing.Remove("mode");
        Assert.False(GraphicsCommandRequest.TryBuild("graphics.clock.control",Request("\"action\":\"configure\",\"expectedClockRevision\":1,\"configuration\":"+missing.ToJsonString()),Observed,out _,out _,out _));
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.clock.control",Request("\"action\":\"pause\""),Observed,out _,out _,out _));
    }

    [Theory]
    [InlineData("compact-solid")] [InlineData("minimal-accent")] [InlineData("broadcast")]
    public void ExistingLooksSendOnlyNativeStylePropertiesAndPreserveAnExplicitBlank(string preset)
    {
        var fields=JsonSerializer.SerializeToElement(new {name="Ada",title=""});var draft=new NativeGraphicsDraft(preset,1,fields,LowerThirdAppearance.FromPreset(preset),new());
        var wire=draft.StyleWire();Assert.False(wire.TryGetProperty("isValid",out _));
        var request=Request("\"templateId\":\""+preset+"\",\"templateVersion\":1,\"scope\":\"global\",\"fields\":"+fields.GetRawText()+",\"style\":"+wire.GetRawText());
        Assert.True(GraphicsCommandRequest.TryBuild("graphics.instance.create",request,Observed,out var command,out _,out _));
        Assert.Equal("",command!.ExtensionData!["graphics"].GetProperty("fields").GetProperty("title").GetString());
        Assert.Equal(preset,draft.ForPreview().Style.GetProperty("preset").GetString());
    }

}
