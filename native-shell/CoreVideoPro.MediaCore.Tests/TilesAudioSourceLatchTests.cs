using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

/// <summary>
/// #478 R3: a Tiles member of a scene on a bus stays an AUDIO source (camera on or off) until
/// that scene leaves the bus or they leave the meeting.
/// </summary>
public sealed class TilesAudioSourceLatchTests
{
    private static MediaCoreTilesLayerWire Wall(string sceneId, params string[] members) =>
        new($"tiles:{sceneId}", 0, members.Select(id => $"zoom:{id}").ToList(), "16:9", 16.0 / 9.0, 1, 1, "#000000");

    private static readonly HashSet<string> Everyone = ["a", "b", "comms", "c"];

    [Fact]
    public void APanelistOnTheProgramWallWhoTurnsTheirCameraOffLosesVideoButKeepsAudio()
    {
        var latch = new TilesAudioSourceLatch();
        ZoomMediaSpinePayloadBuilder.BuildInput Input(bool aCameraOn, MediaCoreTilesLayerWire wall) => new()
        {
            EngineRunning = true,
            Participants =
            [
                new MediaCoreParticipantWire("a", "A", "guest", "main", "Main", false, false, false, 0,
                    aCameraOn ? "live" : "video-off"),
                new MediaCoreParticipantWire("b", "B", "guest", "main", "Main", false, false, false, 0, "live")
            ],
            ProgramTilesLayer = wall,
            StickyAudioParticipantIds = latch.Observe("gallery", wall, null, null, Everyone)
        };

        // Camera on: a is a member of the Program gallery.
        var before = ZoomMediaSpinePayloadBuilder.Build(Input(aCameraOn: true, Wall("gallery", "a", "b")));
        Assert.Contains("a", VideoIds(before));
        Assert.Contains("a", AudioIds(before));

        // Camera off: TilesLayerPayloadBuilder drops a from the members (it filters video-off).
        var after = ZoomMediaSpinePayloadBuilder.Build(Input(aCameraOn: false, Wall("gallery", "b")));
        Assert.DoesNotContain("a", VideoIds(after));
        Assert.Contains("a", AudioIds(after));
    }

    [Fact]
    public void TheLatchClearsWhenTheSceneLeavesTheBusOrTheGuestLeavesTheMeeting()
    {
        var latch = new TilesAudioSourceLatch();
        Assert.Equal(["a", "b"], latch.Observe("gallery", Wall("gallery", "a", "b"), null, null, Everyone));
        // a's camera goes off: still latched.
        Assert.Equal(["a", "b"], latch.Observe("gallery", Wall("gallery", "b"), null, null, Everyone));
        // b leaves the meeting.
        Assert.Equal(["a"], latch.Observe("gallery", Wall("gallery"), null, null, new HashSet<string> { "a" }));
        // A Take puts a different scene on Program: the latch starts over.
        Assert.Equal(["c"], latch.Observe("other", Wall("other", "c"), null, null, Everyone));
        // A non-Tiles scene on Program: nothing latched.
        Assert.Empty(latch.Observe("routes", null, null, null, Everyone));
    }

    [Fact]
    public void ANeverOnCameraParticipantIsNeverLatchedSoNeverAudible()
    {
        // Eligible membership filters video-off, so a comms line that never turns a camera on is
        // never a member, never latched, never a source.
        var latch = new TilesAudioSourceLatch();
        var sticky = latch.Observe("gallery", Wall("gallery", "a"), null, null, Everyone);
        Assert.DoesNotContain("comms", sticky);

        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants =
            [
                new MediaCoreParticipantWire("a", "A", "guest", "main", "Main", false, false, false, 0, "live"),
                new MediaCoreParticipantWire("comms", "OH Comms 1", "guest", "main", "Main", false, false, false, 0, "video-off")
            ],
            ProgramTilesLayer = Wall("gallery", "a"),
            StickyAudioParticipantIds = sticky
        });
        Assert.DoesNotContain("comms", AudioIds(payload));
    }

    [Fact]
    public void EachBusLatchesItsOwnScene()
    {
        var latch = new TilesAudioSourceLatch();
        Assert.Equal(["a", "b"], latch.Observe("pgm", Wall("pgm", "a"), "pvw", Wall("pvw", "b"), Everyone));
        // Preview is re-cued to another scene; Program's latch is untouched.
        Assert.Equal(["a", "c"], latch.Observe("pgm", Wall("pgm"), "pvw2", Wall("pvw2", "c"), Everyone));
    }

    private static List<string> VideoIds(Dictionary<string, object?> payload) => Ids(payload, "participant-video");

    private static List<string> AudioIds(Dictionary<string, object?> payload) => Ids(payload, "participant-audio");

    private static List<string> Ids(Dictionary<string, object?> payload, string kind) =>
        Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"])
            .Where(subscription => subscription["kind"]?.ToString() == kind)
            .Select(subscription => subscription["participantId"]?.ToString() ?? "")
            .ToList();
}
