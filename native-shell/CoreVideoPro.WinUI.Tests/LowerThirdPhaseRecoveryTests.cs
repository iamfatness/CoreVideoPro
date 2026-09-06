using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public sealed class LowerThirdPhaseRecoveryTests
{
    private static LowerThirdKeyState Key(string phase) => new()
    {
        SourceId = "guest", SourceName = "Guest", Title = "Presenter", Org = "Studio",
        Position = "lower-left", Phase = phase, Enabled = true
    };

    [Theory]
    [InlineData("building-in", "on-air", true)]
    [InlineData("building-out", "hidden", false)]
    public void FailedPhaseProducesStableNativeCommandsOnEverySubsequentSync(string phase, string expected, bool enabled)
    {
        var recovered = LowerThirdPhaseRecovery.SettleDesiredState(Key(phase));
        for (var tick = 0; tick < 3; tick++)
        {
            var commands = MediaCoreCommandBuilder.BuildSyncCommands(new MediaCoreProductionSyncContext
            {
                ActiveSceneId = "scene",
                LowerThirdKey = recovered.IsVisible ? new MediaCoreLowerThirdKeyWire(
                    recovered.SourceId, recovered.SourceName, recovered.Title, recovered.Org,
                    recovered.Position, recovered.Phase, recovered.Enabled, recovered.BuildInMs, recovered.BuildOutMs) : null
            });
            var command = Assert.Single(commands.Where(command => command.Type == "set-overlay-asset" &&
                command.ExtensionData!["overlayId"].GetString() == "key:lower-third"));
            Assert.Equal(expected, command.ExtensionData!["keyPhase"].GetString());
            Assert.Equal(enabled, command.ExtensionData["enabled"].GetBoolean());
        }
        Assert.Equal(recovered, LowerThirdPhaseRecovery.SettleDesiredState(recovered));
    }

    [Fact]
    public void LocalRecoveryDoesNotConfirmDeliveryAndRejectsOldOrAnimatingSnapshot()
    {
        var desired = LowerThirdPhaseRecovery.SettleDesiredState(Key("building-in"));
        Assert.False(LowerThirdPhaseRecovery.IsObserved(desired, null));
        Assert.False(LowerThirdPhaseRecovery.IsObserved(desired, State("other", "on-air")));
        Assert.False(LowerThirdPhaseRecovery.IsObserved(desired, State("guest", "building-in")));
        Assert.True(LowerThirdPhaseRecovery.IsObserved(desired, State("guest", "on-air")));
    }

    [Fact]
    public void KeyOutRemainsPendingUntilNativeKeyIsRetired()
    {
        var desired = LowerThirdPhaseRecovery.SettleDesiredState(Key("building-out"));
        Assert.False(LowerThirdPhaseRecovery.IsObserved(desired, State("guest", "building-out")));
        Assert.True(LowerThirdPhaseRecovery.IsObserved(desired,
            new NativeMediaCoreOverlayState { Status = "idle", Summary = "No overlays" }));
    }

    private static NativeMediaCoreOverlayState State(string sourceId, string phase) => new()
    {
        Status = "ready", Summary = "Overlay state",
        Overlays = [new NativeMediaCoreOverlayAssetState
        {
            OverlayId = "key:lower-third", Kind = "lower-third", Position = "lower-third",
            SourceId = sourceId, SourceName = "Guest", Title = "Presenter", Org = "Studio",
            KeyPosition = "lower-left", KeyPhase = phase, Keyer = "downstream", Visible = true
        }]
    };
}
