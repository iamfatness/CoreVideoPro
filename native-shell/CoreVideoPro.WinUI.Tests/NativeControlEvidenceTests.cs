using System.Text.Json;
using CommunityToolkit.Mvvm.Input;
using CoreVideoPro.Control;
using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class NativeControlEvidenceTests
{
    [Fact]
    public void ProgramBufferDiagnosticsSurviveWireMappingAndControlSerializationWithoutDefaultProof()
    {
        const string json = """{"programBuffer":{"activeFrames":3,"occupancy":2,"underruns":1,"delivered":90,"deadlineMisses":4,"outputSequenceGaps":2,"displayPresentationVerified":false}}""";
        var wire = JsonSerializer.Deserialize<NativeMediaCoreWireState>(json, new JsonSerializerOptions(JsonSerializerDefaults.Web))!;
        var snapshot = CoreVideoPro.MediaCore.Services.NativeMediaCoreStateMapper.MapNativeWireStateToSnapshot([], 0, 0, wire);
        var state = NativeControlEvidence.Apply(ControlState.Empty, snapshot);
        using var serialized = JsonDocument.Parse(JsonSerializer.Serialize(state, new JsonSerializerOptions(JsonSerializerDefaults.Web)));
        var buffer = serialized.RootElement.GetProperty("nativeProgramBuffer");
        Assert.Equal(3, buffer.GetProperty("activeFrames").GetInt32());
        Assert.Equal(2, buffer.GetProperty("occupancy").GetInt32());
        Assert.Equal(1, buffer.GetProperty("underruns").GetInt32());
        Assert.Equal(90, buffer.GetProperty("delivered").GetInt32());
        Assert.Equal(4, buffer.GetProperty("deadlineMisses").GetInt32());
        Assert.Equal(2, buffer.GetProperty("outputSequenceGaps").GetInt32());
        Assert.False(buffer.GetProperty("displayPresentationVerified").GetBoolean());
        Assert.False(buffer.TryGetProperty("destinationCompletionVerified", out _));
        Assert.Null(NativeControlEvidence.Apply(state, null).NativeProgramBuffer);
    }

    [Fact]
    public void NativeEvidenceDoesNotEchoDesiredState()
    {
        var desired = new ControlState { ActiveSceneId = "desired", PreviewSceneId = "queued", LowerThirdOnAir = true };
        var native = new NativeMediaCoreStateSnapshot
        {
            SceneId = "observed", ProgramFrameCount = 42,
            PreviewScene = new() { SceneId = "native-preview", Composite = true },
            OverlayState = new()
            {
                Status = "ready", Summary = "native", Overlays = [new()
                {
                    OverlayId = "unrelated-graphic", Kind = "lower-third", Visible = true,
                    Position = "bottom-left", KeyPosition = "downstream", KeyPhase = "on-air", Keyer = "lower-third"
                }, new()
                {
                    OverlayId = "key:lower-third", Kind = "lower-third", Position = "bottom-left",
                    KeyPosition = "downstream", KeyPhase = "hidden", Keyer = "lower-third", Visible = false, SourceId = "zoom:actual"
                }]
            }
        };
        var state = NativeControlEvidence.Apply(desired, native);
        Assert.Equal("desired", state.ActiveSceneId);
        Assert.True(state.LowerThirdOnAir);
        Assert.Equal("observed", state.NativeActiveSceneId);
        Assert.Equal("native-preview", state.NativePreviewSceneId);
        Assert.Equal("hidden", state.NativeLowerThirdPhase);
        Assert.Equal("zoom:actual", state.NativeLowerThirdSourceId);
        Assert.False(state.NativeLowerThirdVisible);
        Assert.Equal(42, state.NativeProgramFrameCount);
    }

    [Fact]
    public void MissingNativeSnapshotClearsObservationsRatherThanReportingIdleAsProof()
    {
        var state = NativeControlEvidence.Apply(new ControlState { NativeActiveSceneId = "stale", NativeLowerThirdVisible = true }, null);
        Assert.Null(state.NativeActiveSceneId);
        Assert.Null(state.NativeRenderedSceneId);
        Assert.Null(state.NativeRenderPlanId);
        Assert.Null(state.NativeProgramVideoSources);
        Assert.Null(state.NativeLowerThirdSourceId);
        Assert.Null(state.NativePreviewSceneId);
        Assert.Null(state.NativeLowerThirdVisible);
        Assert.Null(state.NativeLowerThirdPhase);
        Assert.Null(state.NativeProgramFrameCount);
    }

    [Fact]
    public void ExistingPreviewSceneWireFieldDeserializesIntoEvidence()
    {
        const string json = """{"sceneId":"program","previewScene":{"sceneId":"preview","routeCount":2,"layerCount":2,"composite":true}}""";
        var snapshot = JsonSerializer.Deserialize<NativeMediaCoreStateSnapshot>(json, new JsonSerializerOptions(JsonSerializerDefaults.Web));
        var state = NativeControlEvidence.Apply(ControlState.Empty, snapshot);
        Assert.Equal("program", state.NativeActiveSceneId);
        Assert.Equal("preview", state.NativePreviewSceneId);
    }

    [Fact]
    public void MagicApiInvokesTheExistingCommandOnlyWhenAvailable()
    {
        var available = false;
        var calls = 0;
        var command = new RelayCommand(() => calls++, () => available);
        Assert.False(StudioControlSurface.RunMagicScene(command).Ok);
        Assert.Equal(0, calls);
        available = true;
        Assert.True(StudioControlSurface.RunMagicScene(command).Ok);
        Assert.Equal(1, calls);
    }
    [Fact]
    public void LastRenderedFrameRemainsDistinctFromAcknowledgedScene()
    {
        var snapshot = new NativeMediaCoreStateSnapshot
        {
            SceneId = "new-scene",
            ProgramFrame = new()
            {
                SceneId = "previous-scene", RenderPlanId = "previous-scene:2:0",
                FrameNumber = 10, Health = "live",
                VideoSources = [new() { LayerId = "first", SourceId = "zoom:actual", ParticipantId = "actual", Kind = "participant-video" }]
            }
        };
        var state = NativeControlEvidence.Apply(ControlState.Empty, snapshot);
        Assert.Equal("new-scene", state.NativeActiveSceneId);
        Assert.Equal("previous-scene", state.NativeRenderedSceneId);
        Assert.Equal("previous-scene:2:0", state.NativeRenderPlanId);
        Assert.Equal("zoom:actual", Assert.Single(state.NativeProgramVideoSources!).SourceId);
        var absentScene = NativeControlEvidence.Apply(ControlState.Empty, snapshot with
        {
            ProgramFrame = snapshot.ProgramFrame with { SceneId = null }
        });
        Assert.Null(absentScene.NativeRenderedSceneId);
        Assert.Equal("previous-scene:2:0", absentScene.NativeRenderPlanId);
    }

}
