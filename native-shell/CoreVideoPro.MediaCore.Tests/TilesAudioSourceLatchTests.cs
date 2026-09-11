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
            StickyAudioParticipantIds = latch.Observe(true, "gallery", wall, null, null, Everyone)
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
    public void TheLatchClearsWhenTheSceneLeavesBothBusesOrTheGuestLeavesTheMeeting()
    {
        var latch = new TilesAudioSourceLatch();
        Assert.Equal(["a", "b"], latch.Observe(true, "gallery", Wall("gallery", "a", "b"), null, null, Everyone));
        // a's camera goes off: still latched.
        Assert.Equal(["a", "b"], latch.Observe(true, "gallery", Wall("gallery", "b"), null, null, Everyone));
        // b leaves the meeting.
        Assert.Equal(["a"], latch.Observe(true, "gallery", Wall("gallery"), null, null, new HashSet<string> { "a" }));
        // A different scene is on Program and "gallery" is on neither bus: forgotten.
        Assert.Equal(["c"], latch.Observe(true, "other", Wall("other", "c"), null, null, Everyone));
        // A non-Tiles scene on Program: nothing latched.
        Assert.Empty(latch.Observe(true, "routes", null, null, null, Everyone));
    }

    [Fact]
    public void ATakeThatSwapsTheBusesKeepsACameraOffPanelistAudibleAsTheirGalleryGoesToAir()
    {
        // N5: keyed by SCENE. Gallery B is cued on Preview; panelist m turns their camera off
        // (the membership policy drops them); the Take swaps B onto Program.
        var latch = new TilesAudioSourceLatch();
        var everyone = new HashSet<string> { "m", "x", "y" };
        latch.Observe(true, "A", Wall("A", "x"), "B", Wall("B", "m", "y"), everyone);
        Assert.Contains("m", latch.Observe(true, "A", Wall("A", "x"), "B", Wall("B", "y"), everyone));

        var afterTake = latch.Observe(true, "B", Wall("B", "y"), "A", Wall("A", "x"), everyone);
        Assert.Contains("m", afterTake);
    }

    [Fact]
    public void LeavingTheMeetingOrEngineOffForgetsEverySoAReusedIdIsNotAudible()
    {
        // N5: Zoom reuses per-meeting user ids. A latch that outlived the meeting would make a
        // DIFFERENT person of the next meeting audible.
        var latch = new TilesAudioSourceLatch();
        latch.Observe(true, "gallery", Wall("gallery", "a"), null, null, Everyone);
        Assert.Contains("a", latch.Observe(true, "gallery", Wall("gallery"), null, null, Everyone));

        // Leave (the spine sees no meeting): cleared.
        Assert.Empty(latch.Observe(false, "gallery", Wall("gallery"), null, null, Everyone));
        // Next meeting, same scene on the bus, "a" is a reused id and NOT a member: not audible.
        Assert.Empty(latch.Observe(true, "gallery", Wall("gallery"), null, null, Everyone));

        // Engine off (the spine stops, so it cannot observe the leave): Clear().
        latch.Observe(true, "gallery", Wall("gallery", "b"), null, null, Everyone);
        latch.Clear();
        Assert.Empty(latch.Observe(true, "gallery", Wall("gallery"), null, null, Everyone));
    }

    [Fact]
    public void ANeverOnCameraParticipantIsNeverLatchedSoNeverAudible()
    {
        // Eligible membership filters video-off, so a comms line that never turns a camera on is
        // never a member, never latched, never a source.
        var latch = new TilesAudioSourceLatch();
        var sticky = latch.Observe(true, "gallery", Wall("gallery", "a"), null, null, Everyone);
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
    public void AVideoOnlyPresentSetForgetsACameraOffPanelist_ProductionMustNotObserveThatWay()
    {
        // #485 review: RoomVideoParticipants drops FeedHealth.VideoOff. Observe treats
        // absence as "left the meeting" and cannot re-latch them (Tiles already dropped
        // the camera-off member). Spine must Observe with the full in-room roster.
        var latch = new TilesAudioSourceLatch();
        latch.Observe(true, "gallery", Wall("gallery", "a", "b"), null, null, Everyone);
        var videoOnly = new HashSet<string>(StringComparer.Ordinal) { "b", "comms", "c" };
        Assert.DoesNotContain("a", latch.Observe(true, "gallery", Wall("gallery", "b"), null, null, videoOnly));

        var spine = new TilesAudioSourceLatch();
        spine.Observe(true, "gallery", Wall("gallery", "a", "b"), null, null, Everyone);
        Assert.Contains("a", spine.Observe(true, "gallery", Wall("gallery", "b"), null, null, Everyone));
    }

    [Fact]
    public void AnUnknownMeetingStateTickNeverWipesTheLatch()
    {
        // #478 L5: a tick with no (or a synthesized) snapshot must not read as "left the
        // meeting": a camera-off panelist cannot be re-latched until their camera returns.
        var latch = new TilesAudioSourceLatch();
        latch.Observe(true, "gallery", Wall("gallery", "a", "b"), null, null, Everyone);
        Assert.Equal(["a", "b"], latch.Observe(null, "gallery", Wall("gallery", "b"), null, null, Everyone));
        Assert.Equal(["a", "b"], latch.Observe(true, "gallery", Wall("gallery", "b"), null, null, Everyone));
        // A KNOWN "not in a meeting" still clears.
        Assert.Empty(latch.Observe(false, "gallery", Wall("gallery", "b"), null, null, Everyone));
    }

    [Fact]
    public void EachSceneOnABusKeepsItsOwnLatch()
    {
        var latch = new TilesAudioSourceLatch();
        Assert.Equal(["a", "b"], latch.Observe(true, "pgm", Wall("pgm", "a"), "pvw", Wall("pvw", "b"), Everyone));
        // Preview is re-cued to another scene: "pvw" is on neither bus and is forgotten;
        // Program's scene is untouched.
        Assert.Equal(["a", "c"], latch.Observe(true, "pgm", Wall("pgm"), "pvw2", Wall("pvw2", "c"), Everyone));
    }

    private static List<string> VideoIds(Dictionary<string, object?> payload) => Ids(payload, "participant-video");

    private static List<string> AudioIds(Dictionary<string, object?> payload) => Ids(payload, "participant-audio");

    private static List<string> Ids(Dictionary<string, object?> payload, string kind) =>
        Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"])
            .Where(subscription => subscription["kind"]?.ToString() == kind)
            .Select(subscription => subscription["participantId"]?.ToString() ?? "")
            .ToList();
}
