using CoreVideoPro.MediaCore.Models;
using CoreVideoPro.MediaCore.Services;
using Xunit;

namespace CoreVideoPro.MediaCore.Tests;

public sealed class ZoomMediaSpinePayloadBuilderTests
{
    [Fact]
    public void DefaultCapacityRequestsAllTenShowVideoSources()
    {
        var guests = Enumerable.Range(1, 10).Select(index => Guest(index.ToString(), $"Guest {index}")).ToList();
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants = guests,
            Multiview = Wall(guests.Select((guest, slot) => (slot, guest.Id)).ToArray())
        });

        Assert.Equal(10, Video(payload).Count);
    }

    [Fact]
    public void BuildRequestsVideoAndAudioSubscriptionsForParticipants()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants =
            [
                new MediaCoreParticipantWire("p1", "Alex", "host", "main", "Main", true, false, false, 70, "live"),
                new MediaCoreParticipantWire("p2", "Sam", "guest", "main", "Main", false, true, false, 0, "live")
            ],
            ProgramSceneRoutes = [Route("p1")],
            PreviewSceneRoutes = [Route("p2")]
        });

        Assert.False((bool)payload["blocked"]!);
        var participants = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["participants"]);
        Assert.Equal(2, participants.Count);
        var subscriptions = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"]);
        Assert.Equal(5, subscriptions.Count);
        Assert.Contains(subscriptions, subscription =>
            subscription["kind"]?.ToString() == "meeting-audio" &&
            subscription["purpose"]?.ToString() == "program");
        Assert.Equal(2, subscriptions.Count(subscription => subscription["kind"]?.ToString() == "participant-video"));
        Assert.Equal(2, subscriptions.Count(subscription => subscription["kind"]?.ToString() == "participant-audio"));
    }

    [Fact]
    public void BuildRequestsMeetingMixWhenVideoSubscriptionsAreDisabled()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            MaxVideoSubscriptions = 0,
            Participants =
            [
                new MediaCoreParticipantWire("p1", "Alex", "host", "main", "Main", true, false, false, 70, "live")
            ],
            ProgramSceneRoutes = [Route("p1")]
        });

        var subscriptions = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"]);
        Assert.Contains(subscriptions, subscription => subscription["kind"]?.ToString() == "meeting-audio");
        Assert.DoesNotContain(subscriptions, subscription => subscription["kind"]?.ToString() == "participant-video");
    }

    [Fact]
    public void ProgramAndPreviewRoutesOutrankTheWallAndTheActiveSpeakerDoesNotJumpTheQueue()
    {
        var participants = Enumerable.Range(1, 10)
            .Select(index => Guest($"p{index}", $"Guest {index}", speaker: index == 1))
            .ToList();
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            MaxVideoSubscriptions = 3,
            Participants = participants,
            ProgramSceneRoutes = [Route("p9")],
            PreviewSceneRoutes = [Route("p10")],
            Multiview = Wall((0, "p1"), (1, "p2"), (2, "p3"))
        });

        var video = Video(payload);
        Assert.Equal(["p9", "p10", "p1"], video.Select(Pid).ToArray());
        Assert.Equal(["program", "preview", "multiview"], video.Select(Purpose).ToArray());
    }

    // ---------------------------------------------------------------------------------------
    // #478, live 2026-09-11 (12-person meeting, beta-2026-09-10-70f9027): Alexander, a wall
    // guest with his camera on, had NO video subscription and froze whenever he was not in
    // Preview or Program, while three camera-OFF participants and a camera-on participant who
    // was on nothing held live subscriptions. The owner's rule: only sources that are sources.
    // ---------------------------------------------------------------------------------------

    private const string Host = "16778240";
    private const string OhComms1 = "16783360";        // camera off, early joiner
    private const string OffWallCameraOn = "16788480"; // camera on, routed nowhere
    private const string CameraOffB = "16789504";      // camera off, early joiner
    private const string CameraOffC = "16790528";      // camera off, early joiner
    private const string ProgramGuest = "16791552";
    private const string PreviewGuest = "16792576";
    private const string Alexander = "50332672";       // wall slot 2, joined LAST

    private static List<MediaCoreParticipantWire> LiveRoster(string? speaker) =>
    [
        Guest(Host, "OH Host", speaker: speaker == Host),
        Guest(OhComms1, "OH Comms 1", videoOn: false, speaker: speaker == OhComms1),
        Guest(OffWallCameraOn, "Off-wall guest", speaker: speaker == OffWallCameraOn),
        Guest(CameraOffB, "Camera off B", videoOn: false),
        Guest(CameraOffC, "Camera off C", videoOn: false),
        Guest(ProgramGuest, "Program guest", speaker: speaker == ProgramGuest),
        Guest(PreviewGuest, "Preview guest", speaker: speaker == PreviewGuest),
        Guest("16793600", "Wall C"),
        Guest("16794624", "Wall D"),
        Guest("16795648", "Wall E"),
        Guest("16796672", "Wall F"),
        Guest(Alexander, "Alexander Knight", speaker: speaker == Alexander)
    ];

    private static ZoomMediaSpinePayloadBuilder.BuildInput LiveInput(string? speaker) => new()
    {
        EngineRunning = true,
        MaxVideoSubscriptions = 10,
        Participants = LiveRoster(speaker),
        ProgramSceneRoutes = [Route(ProgramGuest)],
        PreviewSceneRoutes = [Route(PreviewGuest)],
        Multiview = Wall(
            (0, ProgramGuest), (1, PreviewGuest), (2, Alexander),
            (3, "16793600"), (4, "16794624"), (5, "16795648"), (6, "16796672"))
    };

    [Fact]
    public void LiveCase478_AWallGuestWhoJoinedLateIsSubscribedAndNothingUnroutedIs()
    {
        var video = Video(ZoomMediaSpinePayloadBuilder.Build(LiveInput(speaker: Host)));
        var ids = video.Select(Pid).ToList();

        Assert.Contains(Alexander, ids);
        Assert.Equal("multiview", Purpose(video.Single(subscription => Pid(subscription) == Alexander)));
        // Camera off: produces no frames, so it never spends budget.
        Assert.DoesNotContain(OhComms1, ids);
        Assert.DoesNotContain(CameraOffB, ids);
        Assert.DoesNotContain(CameraOffC, ids);
        // Camera on but on no wall slot, no bus and no Tiles scene: not a source.
        Assert.DoesNotContain(OffWallCameraOn, ids);
        // The active speaker is routed nowhere: talking grants nothing.
        Assert.DoesNotContain(Host, ids);
        // The exact order: Program, Preview, then the wall in slot order.
        Assert.Equal(
            [ProgramGuest, PreviewGuest, Alexander, "16793600", "16794624", "16795648", "16796672"],
            ids.ToArray());
    }

    [Fact]
    public void LiveCase478_AudioIsSubscribedForTheSameSourcesAndNobodyElse()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(LiveInput(speaker: Host));
        var audio = Audio(payload);

        Assert.Equal(
            [ProgramGuest, PreviewGuest, Alexander, "16793600", "16794624", "16795648", "16796672"],
            audio.ToArray());
        // Not sources: inaudible in Program, the hardware-switcher model.
        Assert.DoesNotContain(Host, audio);
        Assert.DoesNotContain(OffWallCameraOn, audio);
        Assert.DoesNotContain(OhComms1, audio);
        // The Zoom meeting mix (programMix mode) is untouched by the source rule.
        var subscriptions = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"]);
        var mix = Assert.Single(subscriptions, subscription => subscription["kind"]?.ToString() == "meeting-audio");
        Assert.Equal(Host, mix["participantId"]);
        Assert.Equal("program", mix["purpose"]);
    }

    [Fact]
    public void ANonSourceCameraOnParticipantGetsNeitherVideoNorAudio()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants = [Guest("w1", "Wall"), Guest("x", "Not a source", speaker: true)],
            Multiview = Wall((0, "w1"))
        });

        Assert.DoesNotContain("x", Video(payload).Select(Pid));
        Assert.DoesNotContain("x", Audio(payload));
    }

    [Fact]
    public void ACameraOffWallGuestGetsAudioButNoVideo()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants = [Guest("off", "Camera off", videoOn: false), Guest("on", "Camera on")],
            Multiview = Wall((0, "off"), (1, "on"))
        });

        Assert.Equal(["on"], Video(payload).Select(Pid).ToArray());
        Assert.Equal(["off", "on"], Audio(payload).ToArray());
    }

    [Fact]
    public void AnIsoOnlyGuestGetsBothVideoAndAudio()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants = [Guest("iso", "ISO only"), Guest("x", "Nobody")],
            IsoParticipantIds = ["iso"]
        });

        Assert.Equal(["iso"], Video(payload).Select(Pid).ToArray());
        Assert.Equal(["iso"], Audio(payload).ToArray());
    }

    [Fact]
    public void TheVideoCapNeverCostsASourceItsAudio()
    {
        var guests = Enumerable.Range(0, 12).Select(index => Guest($"g{index}", $"Guest {index}")).ToList();
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            MaxVideoSubscriptions = 10,
            Participants = guests,
            Multiview = Wall(Enumerable.Range(0, 12).Select(slot => (slot, $"g{slot}")).ToArray())
        });

        Assert.Equal(10, Video(payload).Count);
        Assert.Equal(12, Audio(payload).Count);
    }

    [Fact]
    public void LiveCase478_WhoIsTalkingNeverChangesTheVideoSubscriptionSet()
    {
        static string Signature(ZoomMediaSpinePayloadBuilder.BuildInput input) => string.Join(
            "|",
            Video(ZoomMediaSpinePayloadBuilder.Build(input)).Select(subscription => $"{Pid(subscription)}:{Purpose(subscription)}"));

        var baseline = Signature(LiveInput(speaker: Host));
        // The core picks resolution from purpose, so an unchanged (id, purpose) list is what
        // proves an active-speaker flip can no longer re-subscribe a guest at a new resolution.
        Assert.Equal(baseline, Signature(LiveInput(speaker: Alexander)));
        Assert.Equal(baseline, Signature(LiveInput(speaker: ProgramGuest)));
        Assert.Equal(baseline, Signature(LiveInput(speaker: OffWallCameraOn)));
        Assert.Equal(baseline, Signature(LiveInput(speaker: null)));
    }

    [Fact]
    public void ACameraOffGuestGetsNoVideoEvenWhenRouted()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants = [Guest("a", "A", videoOn: false), Guest("b", "B", videoOn: false), Guest("c", "C")],
            ProgramSceneRoutes = [Route("a")],
            Multiview = Wall((0, "b"), (1, "c"))
        });

        Assert.Equal(["c"], Video(payload).Select(Pid).ToArray());
        // Camera-off is not a budget shortfall: there is nothing to warn about.
        Assert.Empty(Shortfall(payload));
    }

    [Fact]
    public void TheLiveRosterCameraOffFlagIsWhatTheBudgetSees()
    {
        // StudioViewModel.BuildSpinePayload builds the spine roster from the engine snapshot
        // through this mapping. It used to pass raw NetworkQuality, so VideoOn=false never
        // reached the builder and every camera-off guest read as live.
        var cameraOff = new RawParticipantEvent { UserId = "a", DisplayName = "A", VideoOn = false, NetworkQuality = "good" };
        var cameraOn = new RawParticipantEvent { UserId = "b", DisplayName = "B", VideoOn = true, NetworkQuality = "low" };
        Assert.Equal("video-off", LiveProductionSync.NormalizeFeedHealthLabel(cameraOff));
        Assert.Equal("low-resolution", LiveProductionSync.NormalizeFeedHealthLabel(cameraOn));

        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants =
            [
                new MediaCoreParticipantWire("a", "A", "guest", "main", "Main", false, false, false, 0,
                    LiveProductionSync.NormalizeFeedHealthLabel(cameraOff)),
                new MediaCoreParticipantWire("b", "B", "guest", "main", "Main", false, false, false, 0,
                    LiveProductionSync.NormalizeFeedHealthLabel(cameraOn))
            ],
            Multiview = Wall((0, "a"), (1, "b"))
        });
        Assert.Equal(["b"], Video(payload).Select(Pid).ToArray());
    }

    [Fact]
    public void TilesMembersOfBothBusesAreSubscribedBecauseATilesSceneSendsNoRoutes()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants =
            [
                Guest("w1", "Wall 1"), Guest("t1", "Tile 1"), Guest("t2", "Tile 2"),
                Guest("bg", "Background guest"), Guest("pv", "Preview tile"), Guest("x", "Nobody")
            ],
            ProgramSceneRoutes = [],
            ProgramTilesLayer = Tiles(["zoom:t1", "zoom:t2", "capture:cam-1"], backgroundSourceId: "zoom:bg"),
            PreviewSceneRoutes = [],
            PreviewTilesLayer = Tiles(["zoom:t2", "zoom:pv"]),
            Multiview = Wall((0, "w1"))
        });

        var video = Video(payload);
        Assert.Equal(["t1", "t2", "bg", "pv", "w1"], video.Select(Pid).ToArray());
        Assert.Equal(
            ["program-tiles", "program-tiles", "program-tiles", "preview-tiles", "multiview"],
            video.Select(Purpose).ToArray());
        Assert.DoesNotContain("x", video.Select(Pid));
    }

    [Fact]
    public void AFixedBusRouteSetsTheFullResolutionPurposeWhateverTierListsTheGuestFirst()
    {
        // t1 sits on the Program Tiles wall (the first tier to list it) AND is the fixed
        // Preview route. Its purpose must be "preview" so the core keeps it at the 1080p tier;
        // the tier decides only its place in the budget.
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Participants = [Guest("t1", "Tile 1"), Guest("t2", "Tile 2")],
            ProgramTilesLayer = Tiles(["zoom:t1", "zoom:t2"]),
            PreviewSceneRoutes = [Route("t1")]
        });

        var video = Video(payload);
        Assert.Equal(["t1", "t2"], video.Select(Pid).ToArray());
        Assert.Equal(["preview", "program-tiles"], video.Select(Purpose).ToArray());
    }

    [Fact]
    public void IsoGuestsGetVideoOnlyWhileIsosAreArmed()
    {
        static ZoomMediaSpinePayloadBuilder.BuildInput Input(IReadOnlyList<string> iso) => new()
        {
            EngineRunning = true,
            Participants = [Guest("w1", "Wall"), Guest("iso", "ISO only"), Guest("x", "Nobody")],
            Multiview = Wall((0, "w1")),
            IsoParticipantIds = iso
        };

        var armed = Video(ZoomMediaSpinePayloadBuilder.Build(Input(["iso"])));
        Assert.Equal(["w1", "iso"], armed.Select(Pid).ToArray());
        Assert.Equal("iso", Purpose(armed[1]));

        // "Program + ISOs" off: the resolver hands the builder nothing, so nothing is spent.
        var disarmed = Video(ZoomMediaSpinePayloadBuilder.Build(Input([])));
        Assert.Equal(["w1"], disarmed.Select(Pid).ToArray());
    }

    [Fact]
    public void AFollowSpeakerRouteSubscribesTheSpeakerAtAStableTierButOnlyWhileTheirCameraIsOn()
    {
        static ZoomMediaSpinePayloadBuilder.BuildInput Input(bool speakerVideoOn) => new()
        {
            EngineRunning = true,
            Participants = [Guest("w1", "Wall"), Guest("s", "Speaker", videoOn: speakerVideoOn, speaker: true)],
            ProgramSceneRoutes = [Route(null, mode: "active-speaker")],
            Multiview = Wall((0, "w1"))
        };

        var on = Video(ZoomMediaSpinePayloadBuilder.Build(Input(speakerVideoOn: true)));
        Assert.Equal(["s", "w1"], on.Select(Pid).ToArray());
        // 720p tier: a follow route's participant changes with whoever talks, so a 1080p
        // purpose here would re-subscribe on every speaker change.
        Assert.Equal("active-speaker", Purpose(on[0]));

        var off = Video(ZoomMediaSpinePayloadBuilder.Build(Input(speakerVideoOn: false)));
        Assert.Equal(["w1"], off.Select(Pid).ToArray());
    }

    [Fact]
    public void AWallGuestLeftOutByTheBudgetIsLoudOnTheTileAndInTheWarnings()
    {
        var guests = Enumerable.Range(0, 11).Select(index => Guest($"g{index}", $"Guest {index}")).ToList();
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            MaxVideoSubscriptions = 10,
            Participants = guests,
            ProgramSceneRoutes = [Route("g10")],
            Multiview = Wall(Enumerable.Range(0, 10).Select(slot => (slot, $"g{slot}")).ToArray())
        });

        var video = Video(payload);
        Assert.Equal(10, video.Count);
        Assert.Equal("g10", Pid(video[0]));
        Assert.DoesNotContain("g9", video.Select(Pid));

        var dropped = Assert.Single(Shortfall(payload));
        Assert.Equal("g9", dropped["participantId"]);
        Assert.Equal("multiview", dropped["purpose"]);

        var warnings = Assert.IsAssignableFrom<IReadOnlyList<string>>(payload["warnings"]);
        Assert.Contains(warnings, warning => warning.Contains("subscription limit (10)", StringComparison.Ordinal));

        var multiview = Assert.IsType<Dictionary<string, object?>>(payload["multiview"]);
        var sources = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(multiview["sources"]);
        Assert.Equal("Guest g9 · no video: subscription limit (10)", sources[9]["label"]);
        Assert.Equal("Guest g0", sources[0]["label"]);
    }

    [Fact]
    public void BuildOmitsMultiviewWhenLayoutIsNull()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true
        });

        Assert.False(payload.ContainsKey("multiview"));
    }

    [Fact]
    public void BuildCarriesMultiviewLayoutWhenProvided()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = true,
            Multiview = new MediaCoreMultiviewLayout(
                1920,
                1080,
                2,
                1,
                [
                    new MediaCoreMultiviewSourceWire("participant:p1", "zoom", 0, "Alex", ParticipantId: "p1"),
                    new MediaCoreMultiviewSourceWire("capture:cam1", "capture", 1, "Cam 1", CaptureDeviceId: "cam1")
                ])
        });

        var multiview = Assert.IsType<Dictionary<string, object?>>(payload["multiview"]);
        Assert.Equal(1920, multiview["canvasWidth"]);
        Assert.Equal(1080, multiview["canvasHeight"]);
        Assert.Equal(2, multiview["cols"]);
        Assert.Equal(1, multiview["rows"]);

        var sources = Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(multiview["sources"]);
        Assert.Equal(2, sources.Count);
        Assert.Equal("participant:p1", sources[0]["sourceId"]);
        Assert.Equal("zoom", sources[0]["kind"]);
        Assert.Equal("p1", sources[0]["participantId"]);
        Assert.Equal(0, sources[0]["slot"]);
        Assert.Equal("capture", sources[1]["kind"]);
        Assert.Equal("cam1", sources[1]["captureDeviceId"]);
    }

    [Fact]
    public void BuildBlocksWhenEngineIsOff()
    {
        var payload = ZoomMediaSpinePayloadBuilder.Build(new ZoomMediaSpinePayloadBuilder.BuildInput
        {
            EngineRunning = false,
            SdkRuntimeReady = false
        });

        Assert.True((bool)payload["blocked"]!);
        var warnings = Assert.IsAssignableFrom<IReadOnlyList<string>>(payload["warnings"]);
        Assert.Contains(warnings, warning => warning.Contains("Media core is not running", StringComparison.Ordinal));
    }

    private static MediaCoreParticipantWire Guest(string id, string name, bool videoOn = true, bool speaker = false) =>
        new(id, name, "guest", "main", "Main", speaker, false, false, 0, videoOn ? "live" : "video-off");

    private static MediaCoreSceneRouteWire Route(string? participantId, string mode = "fixed") =>
        new($"route-{participantId ?? mode}", mode, "isolated", participantId);

    private static MediaCoreMultiviewLayout Wall(params (int Slot, string ParticipantId)[] slots) =>
        new(1920, 1080, 5, 2, slots
            .Select(slot => new MediaCoreMultiviewSourceWire(
                $"zoom:{slot.ParticipantId}", "zoom", slot.Slot, $"Guest {slot.ParticipantId}",
                ParticipantId: slot.ParticipantId))
            .ToList());

    private static MediaCoreTilesLayerWire Tiles(IReadOnlyList<string> members, string backgroundSourceId = "") =>
        new("tiles:scene", 0, members, "16:9", 16.0 / 9.0, 1, 1, "#000000", BackgroundSourceId: backgroundSourceId);

    private static List<Dictionary<string, object?>> Video(Dictionary<string, object?> payload) =>
        Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"])
            .Where(subscription => subscription["kind"]?.ToString() == "participant-video")
            .ToList();

    private static List<string> Audio(Dictionary<string, object?> payload) =>
        Assert.IsAssignableFrom<IReadOnlyList<Dictionary<string, object?>>>(payload["subscriptions"])
            .Where(subscription => subscription["kind"]?.ToString() == "participant-audio")
            .Select(Pid)
            .ToList();

    private static IReadOnlyList<Dictionary<string, object?>> Shortfall(Dictionary<string, object?> payload) =>
        payload.TryGetValue("videoSubscriptionShortfall", out var value) &&
        value is IReadOnlyList<Dictionary<string, object?>> list
            ? list
            : [];

    private static string Pid(Dictionary<string, object?> subscription) => subscription["participantId"]?.ToString() ?? "";

    private static string Purpose(Dictionary<string, object?> subscription) => subscription["purpose"]?.ToString() ?? "";
}
