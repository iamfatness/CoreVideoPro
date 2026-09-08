using System.Text.Json;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>Pure-projection tests for <see cref="OhgSnapshotProjection"/> — turning the
/// show-engine's wire <c>ShowSnapshot</c> (a <see cref="JsonElement"/>, field names exactly as
/// declared in <c>show-engine/src/showSnapshot.ts</c>/<c>contracts.ts</c>/<c>lookDirector.ts</c>/
/// <c>tallyPublisher.ts</c>/<c>overlayDirector.ts</c>/<c>mukanaClient.ts</c>) into the plain .NET
/// view records the shell renders from. No WinUI types are exercised here — <see cref="Project"/>
/// is total (never throws) and the fixtures below include deliberately missing/garbage nodes to
/// prove that.</summary>
public sealed class OhgSnapshotProjectionTests
{
    private static JsonElement Root(string json) => JsonDocument.Parse(json).RootElement.Clone();

    // ---- ProjectsAFullSnapshot --------------------------------------------------------
    //
    // Fixture invariants (rule 1):
    //   - slots.length == capacity == 3 (slot 1 = Alice/host, slot 2 = Bob/panelist, slot 3 = empty hole)
    //   - gallery has exactly 16 cells (cell 1 -> slot 1, cell 2 -> slot 2, cells 3..16 -> slot 0/blank)
    //   - look.boxes reference SEATED slots only (box 1 -> slot 1, box 2 -> slot 2)

    private const string FullSnapshotJson = """
    {
      "revision": 42,
      "panelists": [
        {"participantId":"u1","rawName":"Alice Raw","online":true,"videoOn":true,"audioOn":true,"handRaised":false,"zoomRole":0,
         "displayName":"Alice","location":"NYC","pin":"1234","hasMukana":true,"role":"host"},
        {"participantId":"u2","rawName":"Bob Raw","online":true,"videoOn":false,"audioOn":true,"handRaised":true,"zoomRole":0,
         "displayName":"Bob","location":"LA","pin":null,"hasMukana":false,"role":"panelist"}
      ],
      "slots": [
        {"slot":1,"panelist":{"participantId":"u1","rawName":"Alice Raw","online":true,"videoOn":true,"audioOn":true,"handRaised":false,"zoomRole":0,"displayName":"Alice","location":"NYC","pin":"1234","hasMukana":true,"role":"host"}},
        {"slot":2,"panelist":{"participantId":"u2","rawName":"Bob Raw","online":true,"videoOn":false,"audioOn":true,"handRaised":true,"zoomRole":0,"displayName":"Bob","location":"LA","pin":null,"hasMukana":false,"role":"panelist"}},
        {"slot":3,"panelist":null}
      ],
      "gallery": [
        {"cell":1,"slot":1}, {"cell":2,"slot":2}, {"cell":3,"slot":0}, {"cell":4,"slot":0},
        {"cell":5,"slot":0}, {"cell":6,"slot":0}, {"cell":7,"slot":0}, {"cell":8,"slot":0},
        {"cell":9,"slot":0}, {"cell":10,"slot":0}, {"cell":11,"slot":0}, {"cell":12,"slot":0},
        {"cell":13,"slot":0}, {"cell":14,"slot":0}, {"cell":15,"slot":0}, {"cell":16,"slot":0}
      ],
      "queue": {"previous":["1111"],"current":"2222","upcoming":["3333","4444"]},
      "program": {"program":{"kind":"look","lookId":"solo"},"preview":{"kind":"gallery"},"activeSpeakerFollow":true,"activeSpeakerId":"u1"},
      "look": {"lookId":"solo","scenePreset":"SoloPreset","plateTone":"neutral","tallySource":"boxes","hostSlot":1,"readerSlot":null,
               "boxes":[{"box":1,"slot":1},{"box":2,"slot":2}],"nameplates":[],"page":0,"pageCount":1,"boxFill":"queue"},
      "manualBoxes": {"1":1,"2":2},
      "tally": {"mode":"look","onAirSlots":[1,2],"onAirPins":[],"onAirParticipantIds":[]},
      "overlays": {"nameplates":[],"question":{"askerName":"Asker","text":"Q text","tag":"t","votes":5},
                   "headline":{"name":"HL","location":"HLLoc"},"headlineVisible":true},
      "capabilities": {"registry":{"state":"available","detail":null},
                        "handsQueue":{"state":"unavailable","detail":"down"},
                        "questionFeed":{"state":"disabled","detail":null}},
      "health": {"panelists":{"state":"ok","consecutiveFailures":0,"detail":null},
                 "hands":{"state":"dormant","consecutiveFailures":2,"detail":"slow"},
                 "question":{"state":"failing","consecutiveFailures":5,"detail":"down"}},
      "smartGallery": true,
      "unseated": [{"participantId":"u3","rawName":"Carol Raw","online":false,"videoOn":false,"audioOn":false,"handRaised":false,"zoomRole":0,"displayName":"Carol","location":"SF","pin":"5678","hasMukana":true,"role":"panelist"}],
      "pagingRefused": "no queue window",
      "restoreWarnings": ["dropped stale look"]
    }
    """;

    [Fact]
    public void ProjectsAFullSnapshot()
    {
        var view = OhgSnapshotProjection.Project(Root(FullSnapshotJson), out var warnings);

        Assert.Empty(warnings);
        Assert.Equal(42, view.Revision);

        // panelists[] — full roster, each resolved against slots[]
        Assert.Equal(2, view.Panelists.Count);
        var alice = view.Panelists[0];
        Assert.Equal("u1", alice.ParticipantId);
        Assert.Equal("Alice", alice.DisplayName);
        Assert.Equal("NYC", alice.Location);
        Assert.Equal("1234", alice.Pin);
        Assert.True(alice.HasMukana);
        Assert.Equal("host", alice.Role);
        Assert.True(alice.Online);
        Assert.True(alice.VideoOn);
        Assert.True(alice.AudioOn);
        Assert.False(alice.HandRaised);
        Assert.Equal(1, alice.Slot);
        var bob = view.Panelists[1];
        Assert.Equal("u2", bob.ParticipantId);
        Assert.Null(bob.Pin);
        Assert.False(bob.HasMukana);
        Assert.Equal(2, bob.Slot);

        // slots[] — an occupied seat resolves its panelist row; a hole has none; OnAir from tally
        Assert.Equal(3, view.Slots.Count);
        Assert.Equal(1, view.Slots[0].Slot);
        Assert.NotNull(view.Slots[0].Panelist);
        Assert.Equal("Alice", view.Slots[0].Panelist!.DisplayName);
        Assert.True(view.Slots[0].OnAir);
        Assert.Equal(2, view.Slots[1].Slot);
        Assert.True(view.Slots[1].OnAir);
        Assert.Equal(3, view.Slots[2].Slot);
        Assert.Null(view.Slots[2].Panelist);
        Assert.False(view.Slots[2].OnAir);

        // gallery[] — 16 cells, seated slots resolve a display name, blanks are null
        Assert.Equal(16, view.Gallery.Count);
        Assert.Equal(1, view.Gallery[0].Cell);
        Assert.Equal(1, view.Gallery[0].Slot);
        Assert.Equal("Alice", view.Gallery[0].DisplayName);
        Assert.Equal("Bob", view.Gallery[1].DisplayName);
        for (var i = 2; i < 16; i++)
        {
            Assert.Equal(0, view.Gallery[i].Slot);
            Assert.Null(view.Gallery[i].DisplayName);
        }

        // queue
        Assert.Equal(new[] { "1111" }, view.Queue.Previous);
        Assert.Equal("2222", view.Queue.Current);
        Assert.Equal(new[] { "3333", "4444" }, view.Queue.Upcoming);

        // program — wire strings, not raw objects
        Assert.Equal("look:solo", view.Program.Program);
        Assert.Equal("gallery", view.Program.Preview);
        Assert.True(view.Program.ActiveSpeakerFollow);
        Assert.Equal("u1", view.Program.ActiveSpeakerId);

        // look — boxes reference seated slots and resolve a display name
        Assert.NotNull(view.Look);
        Assert.Equal("solo", view.Look!.LookId);
        Assert.Equal("SoloPreset", view.Look.ScenePreset);
        Assert.Equal(1, view.Look.HostSlot);
        Assert.Null(view.Look.ReaderSlot);
        Assert.Equal(2, view.Look.Boxes.Count);
        Assert.Equal(1, view.Look.Boxes[0].Box);
        Assert.Equal(1, view.Look.Boxes[0].Slot);
        Assert.Equal("Alice", view.Look.Boxes[0].DisplayName);
        Assert.Equal(2, view.Look.Boxes[1].Box);
        Assert.Equal("Bob", view.Look.Boxes[1].DisplayName);
        Assert.Equal(0, view.Look.Page);
        Assert.Equal(1, view.Look.PageCount);
        Assert.Equal("queue", view.Look.BoxFill);

        // manualBoxes — an object keyed by box number
        Assert.Equal(2, view.ManualBoxes.Count);
        Assert.Equal(1, view.ManualBoxes[1]);
        Assert.Equal(2, view.ManualBoxes[2]);

        // onAirSlots (raw, top-level)
        Assert.Equal(new[] { 1, 2 }, view.OnAirSlots);

        // overlays
        Assert.Equal("Q text", view.Overlays.QuestionText);
        Assert.Equal("Asker", view.Overlays.QuestionAsker);
        Assert.Equal("HL", view.Overlays.HeadlineName);
        Assert.Equal("HLLoc", view.Overlays.HeadlineLocation);
        Assert.True(view.Overlays.HeadlineVisible);

        // capabilities
        Assert.Equal("available", view.Registry.State);
        Assert.Null(view.Registry.Detail);
        Assert.Equal("unavailable", view.HandsQueue.State);
        Assert.Equal("down", view.HandsQueue.Detail);
        Assert.Equal("disabled", view.QuestionFeed.State);

        // health + worst
        Assert.Equal("ok", view.Health.Panelists);
        Assert.Equal("dormant", view.Health.Hands);
        Assert.Equal("failing", view.Health.Question);
        Assert.Equal("failing", view.Health.Worst);

        // misc top-level scalars/lists
        Assert.True(view.SmartGallery);
        Assert.Single(view.Unseated);
        Assert.Equal("Carol", view.Unseated[0].DisplayName);
        Assert.Null(view.Unseated[0].Slot);
        Assert.Equal("no queue window", view.PagingRefused);
        Assert.Equal(new[] { "dropped stale look" }, view.RestoreWarnings);
    }

    // ---- FormatsEveryProgramSourceKind -------------------------------------------------

    [Fact]
    public void FormatsEveryProgramSourceKind()
    {
        Assert.Equal("look:show.solo", OhgSnapshotProjection.FormatProgramSource(Root("""{"kind":"look","lookId":"show.solo"}""")));
        Assert.Equal("slot:7", OhgSnapshotProjection.FormatProgramSource(Root("""{"kind":"slot","slot":7}""")));
        Assert.Equal("black", OhgSnapshotProjection.FormatProgramSource(Root("""{"kind":"black"}""")));
        Assert.Equal("gallery", OhgSnapshotProjection.FormatProgramSource(Root("""{"kind":"gallery"}""")));
        Assert.Equal("activeSpeaker", OhgSnapshotProjection.FormatProgramSource(Root("""{"kind":"activeSpeaker"}""")));
    }

    // ---- AnEmptySeatIsAHole_AndOnAirComesFromTally -------------------------------------

    private const string HoleAndTallyJson = """
    {
      "slots": [
        {"slot":1,"panelist":{"participantId":"u1","rawName":"Alice Raw","online":true,"videoOn":true,"audioOn":true,"handRaised":false,"zoomRole":0,"displayName":"Alice","location":"NYC","pin":null,"hasMukana":true,"role":"host"}},
        {"slot":2,"panelist":{"participantId":"u2","rawName":"Bob Raw","online":true,"videoOn":true,"audioOn":true,"handRaised":false,"zoomRole":0,"displayName":"Bob","location":"LA","pin":null,"hasMukana":true,"role":"panelist"}},
        {"slot":3,"panelist":null}
      ],
      "tally": {"onAirSlots":[2]}
    }
    """;

    [Fact]
    public void AnEmptySeatIsAHole_AndOnAirComesFromTally()
    {
        var view = OhgSnapshotProjection.Project(Root(HoleAndTallyJson), out _);

        // slot 1 is OCCUPIED but not in tally.onAirSlots -> not on air (never derived from occupancy)
        Assert.NotNull(view.Slots[0].Panelist);
        Assert.False(view.Slots[0].OnAir);

        // slot 2 is occupied AND in tally.onAirSlots -> on air
        Assert.NotNull(view.Slots[1].Panelist);
        Assert.True(view.Slots[1].OnAir);

        // slot 3 is an empty hole and not in tally -> no panelist, not on air
        Assert.Null(view.Slots[2].Panelist);
        Assert.False(view.Slots[2].OnAir);
    }

    // ---- GalleryCellNamesFollowTheSeatedPanelist_BlankIsNull ---------------------------

    private const string GalleryJson = """
    {
      "slots": [
        {"slot":1,"panelist":{"participantId":"u1","rawName":"Alice Raw","online":true,"videoOn":true,"audioOn":true,"handRaised":false,"zoomRole":0,"displayName":"Alice","location":"NYC","pin":null,"hasMukana":true,"role":"host"}},
        {"slot":2,"panelist":null}
      ],
      "gallery": [
        {"cell":1,"slot":1},
        {"cell":2,"slot":2},
        {"cell":3,"slot":0}
      ]
    }
    """;

    [Fact]
    public void GalleryCellNamesFollowTheSeatedPanelist_BlankIsNull()
    {
        var view = OhgSnapshotProjection.Project(Root(GalleryJson), out _);

        Assert.Equal("Alice", view.Gallery[0].DisplayName);   // cell 1 -> slot 1, seated
        Assert.Null(view.Gallery[1].DisplayName);              // cell 2 -> slot 2, unseated hole
        Assert.Null(view.Gallery[2].DisplayName);              // cell 3 -> slot 0, blank
    }

    // ---- WorstMukanaHealthWins ----------------------------------------------------------

    [Fact]
    public void WorstMukanaHealthWins()
    {
        const string okDormantFailing = """
        {"health":{"panelists":{"state":"ok","consecutiveFailures":0,"detail":null},
                    "hands":{"state":"dormant","consecutiveFailures":1,"detail":null},
                    "question":{"state":"failing","consecutiveFailures":9,"detail":null}}}
        """;
        const string okOkDormant = """
        {"health":{"panelists":{"state":"ok","consecutiveFailures":0,"detail":null},
                    "hands":{"state":"ok","consecutiveFailures":0,"detail":null},
                    "question":{"state":"dormant","consecutiveFailures":1,"detail":null}}}
        """;

        var a = OhgSnapshotProjection.Project(Root(okDormantFailing), out _);
        var b = OhgSnapshotProjection.Project(Root(okOkDormant), out _);

        Assert.Equal("failing", a.Health.Worst);
        Assert.Equal("dormant", b.Health.Worst);
    }

    // ---- MissingNodesProjectNeutral_AndAreListedAsWarnings -----------------------------

    [Fact]
    public void MissingNodesProjectNeutral_AndAreListedAsWarnings()
    {
        var view = OhgSnapshotProjection.Project(Root("{}"), out var warnings);

        Assert.Empty(view.Panelists);
        Assert.Empty(view.Slots);
        Assert.Empty(view.Gallery);
        Assert.Empty(view.Queue.Previous);
        Assert.Null(view.Queue.Current);
        Assert.Empty(view.Queue.Upcoming);
        Assert.Equal("black", view.Program.Program);
        Assert.Equal("black", view.Program.Preview);
        Assert.Null(view.Look);
        Assert.Empty(view.ManualBoxes);
        Assert.Empty(view.OnAirSlots);
        Assert.Null(view.Overlays.QuestionText);
        Assert.Null(view.Overlays.HeadlineName);
        Assert.False(view.Overlays.HeadlineVisible);
        Assert.Equal("unavailable", view.Registry.State);
        Assert.Equal("unavailable", view.HandsQueue.State);
        Assert.Equal("unavailable", view.QuestionFeed.State);
        Assert.Equal("failing", view.Health.Panelists);
        Assert.Equal("failing", view.Health.Hands);
        Assert.Equal("failing", view.Health.Question);
        Assert.Equal("failing", view.Health.Worst);
        Assert.False(view.SmartGallery);
        Assert.Empty(view.Unseated);
        Assert.Null(view.PagingRefused);
        Assert.Empty(view.RestoreWarnings);

        Assert.NotEmpty(warnings);
    }

    // ---- NeverThrowsOnGarbage -----------------------------------------------------------

    [Fact]
    public void NeverThrowsOnGarbage()
    {
        const string garbage = """{"slots":"x","gallery":5,"look":[]}""";

        var view = OhgSnapshotProjection.Project(Root(garbage), out var warnings);

        Assert.Empty(view.Slots);
        Assert.Empty(view.Gallery);
        Assert.Null(view.Look);
        Assert.NotEmpty(warnings);
    }

    // ---- LookOptionsFromEngine_ReadsIdAndLabel_SkipsMalformed --------------------------

    [Fact]
    public void LookOptionsFromEngine_ReadsIdAndLabel_SkipsMalformed()
    {
        const string engineJson = """
        {"looks":[
          {"id":"solo","label":"Solo Shot"},
          {"id":"noLabel"},
          {"label":"missingId"},
          {"id":123},
          {}
        ]}
        """;

        var options = OhgSnapshotProjection.LookOptionsFromEngine(Root(engineJson));

        Assert.Equal(2, options.Count);
        Assert.Equal("solo", options[0].Id);
        Assert.Equal("Solo Shot", options[0].Label);
        Assert.Equal("noLabel", options[1].Id);
        Assert.Equal("noLabel", options[1].Label);  // label falls back to id when absent
    }
}
