using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class MixerChannelSetPolicyTests
{
    private const string Host = "16778240";
    private const string OffWall = "16788480";
    private const string ProgramGuest = "16791552";
    private const string PreviewGuest = "16792576";
    private const string Alexander = "50332672";

    [Fact]
    public void LiveCase485_MixerOmitsNonSourceZoomGuestsAndKeepsNonZoomChannels()
    {
        var sources = ZoomSourceSetPolicy.Resolve(LiveSourceInput());
        var zoomIds = ZoomSourceSetPolicy.AudioParticipantIds(sources).ToHashSet(StringComparer.Ordinal);

        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(ProgramGuest, zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(PreviewGuest, zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(Alexander, zoomIds));
        Assert.False(MixerChannelSetPolicy.DisplayOnMixer(Host, zoomIds));
        Assert.False(MixerChannelSetPolicy.DisplayOnMixer(OffWall, zoomIds));

        Assert.True(MixerChannelSetPolicy.DisplayOnMixer("zoom-mix", zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer("local-machine-audio", zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer("media", zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer("capture:cam-1", zoomIds));
    }

    [Fact]
    public void CameraOffWallIsoAndStickySourcesAreDisplayedAndUnroutedCameraOffIsNot()
    {
        var cameraOffWall = "wall-off";
        var cameraOffIso = "iso-off";
        var sticky = "tiles-off";
        var unroutedOff = "comms";
        var sources = ZoomSourceSetPolicy.Resolve(new ZoomSourceSetPolicy.Input
        {
            Participants =
            [
                new ZoomSourceSetPolicy.ParticipantState(ProgramGuest, true, false),
                new ZoomSourceSetPolicy.ParticipantState(cameraOffWall, false, false),
                new ZoomSourceSetPolicy.ParticipantState(cameraOffIso, false, false),
                new ZoomSourceSetPolicy.ParticipantState(sticky, false, false),
                new ZoomSourceSetPolicy.ParticipantState(unroutedOff, false, false)
            ],
            ProgramRoutes = [new MediaCoreSceneRouteWire("r-pgm", "fixed", "isolated", ProgramGuest)],
            WallSources = [new("zoom:" + cameraOffWall, "zoom", 0, "Wall off", ParticipantId: cameraOffWall)],
            IsoParticipantIds = [cameraOffIso],
            StickyAudioParticipantIds = [sticky]
        });
        var zoomIds = ZoomSourceSetPolicy.AudioParticipantIds(sources).ToHashSet(StringComparer.Ordinal);

        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(cameraOffWall, zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(cameraOffIso, zoomIds));
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(sticky, zoomIds));
        Assert.False(MixerChannelSetPolicy.DisplayOnMixer(unroutedOff, zoomIds));
    }

    [Fact]
    public void ANonSourceBecomingASourceIsDisplayed()
    {
        var without = ZoomSourceSetPolicy.AudioParticipantIds(
            ZoomSourceSetPolicy.Resolve(LiveSourceInput(wallHost: false))).ToHashSet(StringComparer.Ordinal);
        Assert.False(MixerChannelSetPolicy.DisplayOnMixer(Host, without));

        var with = ZoomSourceSetPolicy.AudioParticipantIds(
            ZoomSourceSetPolicy.Resolve(LiveSourceInput(wallHost: true))).ToHashSet(StringComparer.Ordinal);
        Assert.True(MixerChannelSetPolicy.DisplayOnMixer(Host, with));
    }

    private static ZoomSourceSetPolicy.Input LiveSourceInput(bool wallHost = false)
    {
        var wall = new List<MediaCoreMultiviewSourceWire>
        {
            new("zoom:" + ProgramGuest, "zoom", 0, "Program", ParticipantId: ProgramGuest),
            new("zoom:" + PreviewGuest, "zoom", 1, "Preview", ParticipantId: PreviewGuest),
            new("zoom:" + Alexander, "zoom", 2, "Alexander", ParticipantId: Alexander)
        };
        if (wallHost)
        {
            wall.Add(new("zoom:" + Host, "zoom", 3, "Host", ParticipantId: Host));
        }

        return new ZoomSourceSetPolicy.Input
        {
            Participants =
            [
                new ZoomSourceSetPolicy.ParticipantState(Host, true, false),
                new ZoomSourceSetPolicy.ParticipantState(OffWall, true, true),
                new ZoomSourceSetPolicy.ParticipantState(ProgramGuest, true, false),
                new ZoomSourceSetPolicy.ParticipantState(PreviewGuest, true, false),
                new ZoomSourceSetPolicy.ParticipantState(Alexander, true, false)
            ],
            ProgramRoutes = [new MediaCoreSceneRouteWire("r-pgm", "fixed", "isolated", ProgramGuest)],
            PreviewRoutes = [new MediaCoreSceneRouteWire("r-pvw", "fixed", "isolated", PreviewGuest)],
            WallSources = wall
        };
    }
}
