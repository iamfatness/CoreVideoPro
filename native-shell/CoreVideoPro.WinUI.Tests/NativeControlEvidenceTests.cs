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
                    KeyPosition = "downstream", KeyPhase = "hidden", Keyer = "lower-third", Visible = false
                }]
            }
        };
        var state = NativeControlEvidence.Apply(desired, native);
        Assert.Equal("desired", state.ActiveSceneId);
        Assert.True(state.LowerThirdOnAir);
        Assert.Equal("observed", state.NativeActiveSceneId);
        Assert.Equal("native-preview", state.NativePreviewSceneId);
        Assert.Equal("hidden", state.NativeLowerThirdPhase);
        Assert.False(state.NativeLowerThirdVisible);
        Assert.Equal(42, state.NativeProgramFrameCount);
    }

    [Fact]
    public void MissingNativeSnapshotClearsObservationsRatherThanReportingIdleAsProof()
    {
        var state = NativeControlEvidence.Apply(new ControlState { NativeActiveSceneId = "stale", NativeLowerThirdVisible = true }, null);
        Assert.Null(state.NativeActiveSceneId);
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
}
