using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeGraphicsStateTests
{
    [Fact]
    public void DraftAcceptedCuePendingCueAndProgramStayDistinctAcrossTheActualWireMapper()
    {
        using var response = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"health":null,"profile":null,"graphics":{
              "schemaVersion":1,"coreEpoch":"test-core","instances":[{
                "instanceId":"featured","revision":4,"cueRevision":2,"programRevision":1,
                "cueRequestedRevision":3,"cuePending":true,"cuePreparation":"preparing","cueWarning":"",
                "fields":{"name":"Private edit","title":""},"cueFields":{"name":"Accepted Cue","title":"Presenter"},
                "programFields":{"name":"Program","title":"Engineer"},
                "cueStyle":{"nameSize":52},"cueAnimations":{"in":{"type":"fade","durationMs":400}},
                "cueBinding":{"sourceId":"zoom:44","instanceId":"guest-one","processEpoch":"meeting-one","generation":1},
                "cueResource":{"backend":"d3d11","width":320,"height":64,"contentKey":"cue-content"}}]}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var instance = Assert.Single(NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire).Graphics!.Instances);
        Assert.Equal(4, instance.Revision); Assert.Equal(2, instance.CueRevision); Assert.Equal(1, instance.ProgramRevision);
        Assert.Equal(3, instance.CueRequestedRevision); Assert.True(instance.CuePending);
        Assert.Equal("preparing", instance.CuePreparation); Assert.Equal("", instance.CueWarning);
        Assert.Equal("", instance.Fields.Title); Assert.Equal("Accepted Cue", instance.CueFields!.Name);
        Assert.Equal("Program", instance.ProgramFields!.Name); Assert.Equal(52, instance.CueStyle!.NameSize);
        Assert.Equal("fade", instance.CueAnimations!.In.Type); Assert.Equal(400, instance.CueAnimations.In.DurationMs);
        Assert.Equal("guest-one", instance.CueBinding!.InstanceId); Assert.Equal("cue-content", instance.CueResource!.ContentKey);
    }
    [Fact]
    public void ResourceBudgetRemainsAnAllocationModelObservationAtStatePublication()
    {
        using var response = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"health":null,"profile":null,"graphics":{
              "schemaVersion":1,"coreEpoch":"test-core","instances":[],
              "resourceBudgetAtStatePublication":{"kind":"allocation-model","cpuBytes":128,"gpuBytes":256,
                "activePreparations":1,"cpuHighWater":512,"gpuHighWater":1024,"preparationHighWater":2,
                "refused":3,"cpuLimit":134217728,"gpuLimit":268435456,"preparationLimit":2}}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var budget = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire).Graphics!.ResourceBudgetAtStatePublication!;
        Assert.Equal("allocation-model", budget.Kind);
        Assert.Equal(128, budget.CpuBytes); Assert.Equal(256, budget.GpuBytes);
        Assert.Equal(134217728, budget.CpuLimit); Assert.Equal(268435456, budget.GpuLimit);
        Assert.Equal(1, budget.ActivePreparations); Assert.Equal(2, budget.PreparationLimit);
        Assert.Equal(512, budget.CpuHighWater); Assert.Equal(1024, budget.GpuHighWater);
        Assert.Equal(2, budget.PreparationHighWater); Assert.Equal(3, budget.Refused);
    }
    [Fact]
    public void ActualWireParserAndMapperPreserveIndependentRevisionsAndBlankTitle()
    {
        using var response = JsonDocument.Parse("""
            {"ok":true,"snapshot":{"health":null,"profile":null,"graphics":{
              "schemaVersion":1,"coreEpoch":"native-epoch","instances":[{
                "instanceId":"featured","templateId":"broadcast","templateVersion":1,
                "scope":"scene","sceneId":"interview","revision":3,"programRevision":2,
                "preparation":"preparing","visibility":"on-air","firstDrawSubmittedFrame":99,
                "cued":true,"showPending":false,"warning":"",
                "identity":{"instanceId":"featured","coreEpoch":"native-epoch","revision":3},
                "binding":{"sourceId":"zoom:44","instanceId":"subject-new","processEpoch":"native:18446744073709551615","generation":2},
                "programBinding":{"sourceId":"zoom:44","instanceId":"subject-old","processEpoch":"native:18446744073709551615","generation":1},
                "bindingState":"bound",
                "preparedResource":{"backend":"d3d11","width":600,"height":96,"contentKey":"18446744073709551615"},
                "programResource":{"backend":"d3d11","width":800,"height":128,"contentKey":"42"},
                "preparationObservation":{"identity":{"instanceId":"featured","coreEpoch":"native-epoch","revision":3},"state":"preparing","warning":""},
                "firstCompositionObservation":{"identity":{"instanceId":"featured","coreEpoch":"native-epoch","revision":2},"frameNumber":99,"evidence":"draw-submitted"},
                "fields":{"name":"Guest","title":""},"style":{"nameSize":65,"titleSize":36},
                "animations":{"in":{"type":"slide","direction":"up","durationMs":400,"easing":"ease-out"}},
                "programAnimations":{"out":{"type":"fade","durationMs":300}},
                "programFields":{"name":"On air","title":"Engineer"},"programStyle":{"nameSize":52}}],
              "sources":[{"identity":{"sourceId":"zoom:44","instanceId":"subject-new","processEpoch":"native:18446744073709551615","generation":2},"displayName":"Guest","videoOn":true}],
              "sourceBindingStatus":"available",
              "lastResult":{"requestId":"update-1","accepted":true,"reason":"revision-accepted"}}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);
        Assert.NotNull(snapshot.Graphics);
        var instance = Assert.Single(snapshot.Graphics.Instances);
        Assert.Equal(3, instance.Revision);
        Assert.Equal(2, instance.ProgramRevision);
        Assert.Equal(2, instance.Binding!.Generation);
        Assert.Equal(1, instance.ProgramBinding!.Generation);
        Assert.Equal("native:18446744073709551615", instance.Binding.ProcessEpoch);
        Assert.Equal("bound", instance.BindingState);
        Assert.Equal("zoom:44", Assert.Single(snapshot.Graphics.Sources).Identity!.SourceId);
        Assert.Equal("available", snapshot.Graphics.SourceBindingStatus);
        Assert.Equal("18446744073709551615", instance.PreparedResource!.ContentKey);
        Assert.Equal(600, instance.PreparedResource.Width);
        Assert.Equal(800, instance.ProgramResource!.Width);
        Assert.Equal("", instance.Fields.Title);
        Assert.Equal(65, instance.Style.NameSize);
        Assert.Equal(36, instance.Style.TitleSize);
        Assert.Equal("On air", instance.ProgramFields!.Name);
        Assert.Equal(52, instance.ProgramStyle!.NameSize);
        Assert.Equal("slide", instance.Animations.In.Type);
        Assert.Equal("up", instance.Animations.In.Direction);
        Assert.Equal(400, instance.Animations.In.DurationMs);
        Assert.Equal("ease-out", instance.Animations.In.Easing);
        Assert.Equal("fade", instance.ProgramAnimations!.Out.Type);
        Assert.Equal(300, instance.ProgramAnimations.Out.DurationMs);
        Assert.Equal("preparing", instance.Preparation);
        Assert.Equal(3, instance.Identity!.Revision);
        Assert.Equal(3, instance.PreparationObservation!.Identity.Revision);
        Assert.Equal("preparing", instance.PreparationObservation.State);
        Assert.Equal(2, instance.FirstCompositionObservation!.Identity.Revision);
        Assert.Equal("draw-submitted", instance.FirstCompositionObservation.Evidence);
        Assert.Equal(99, instance.FirstDrawSubmittedFrame);
        Assert.Equal("native-epoch", snapshot.Graphics.CoreEpoch);
        Assert.True(snapshot.Graphics.LastResult!.Accepted);
    }

    [Fact]
    public void OlderCoreDoesNotInventGraphicsReadiness()
    {
        using var response = JsonDocument.Parse("""{"ok":true,"snapshot":{"health":null,"profile":null}}""");
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        Assert.Null(NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire).Graphics);
    }

    [Fact]
    public void ObservationViewsRetainNativeFactsWithoutTurningAdmissionIntoDelivery()
    {
        const string json = """{"graphics":{"schemaVersion":1,"lastResult":{"accepted":true},"instances":[]}}""";
        var observation = CoreObservationModel.Parse(json);
        foreach (var view in new[] { observation.TypedJson(), observation.QualificationJson(), observation.ControlJson() })
        {
            using var document = JsonDocument.Parse(view);
            Assert.True(document.RootElement.GetProperty("graphics").GetProperty("lastResult").GetProperty("accepted").GetBoolean());
            Assert.False(document.RootElement.GetProperty("graphics").TryGetProperty("delivered", out _));
        }
    }
}
