using System.Text.Json;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class NativeGraphicsStateTests
{
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
                "preparationObservation":{"identity":{"instanceId":"featured","coreEpoch":"native-epoch","revision":3},"state":"preparing","warning":""},
                "firstCompositionObservation":{"identity":{"instanceId":"featured","coreEpoch":"native-epoch","revision":2},"frameNumber":99,"evidence":"draw-submitted"},
                "fields":{"name":"Guest","title":""},"style":{"nameSize":65,"titleSize":36},
                "programFields":{"name":"On air","title":"Engineer"},"programStyle":{"nameSize":52}}],
              "lastResult":{"requestId":"update-1","accepted":true,"reason":"revision-accepted"}}}}
            """);
        var wire = CoreProtocolParser.TryParseWireState(response);
        Assert.NotNull(wire);
        var snapshot = NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot(0, 0, wire);
        Assert.NotNull(snapshot.Graphics);
        var instance = Assert.Single(snapshot.Graphics.Instances);
        Assert.Equal(3, instance.Revision);
        Assert.Equal(2, instance.ProgramRevision);
        Assert.Equal("", instance.Fields.Title);
        Assert.Equal(65, instance.Style.NameSize);
        Assert.Equal(36, instance.Style.TitleSize);
        Assert.Equal("On air", instance.ProgramFields!.Name);
        Assert.Equal(52, instance.ProgramStyle!.NameSize);
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
