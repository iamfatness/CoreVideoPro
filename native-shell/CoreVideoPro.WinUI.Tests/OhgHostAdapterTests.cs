using System.Text.Json;
using CoreVideoPro.ShowEngine;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

/// <summary>
/// Plan 7a Task 10 — <see cref="OhgHostAdapter"/> maps the show engine's host commands onto
/// <see cref="IOhgHostFacade"/> (spec §8's table, row for row), refuses in the engine's own
/// words rather than throwing, and in shadow mode records every command while touching nothing
/// but <see cref="IOhgHostFacade.ReportStatus"/>.
///
/// The wire shapes asserted here are the ones the engine actually emits
/// (<c>show-engine/src/host/stdioHostAdapter.ts</c> + <c>protocol.ts</c>'s Map→pair-array
/// replacer), not a convenient re-imagining of them.
/// </summary>
public sealed class OhgHostAdapterTests
{
    // ---- fixtures -------------------------------------------------------------------

    /// <summary>Records every facade call. In shadow mode the adapter may call NOTHING here but
    /// <see cref="ReportStatus"/>; a violation is recorded (never thrown — the adapter swallows
    /// exceptions by contract, so a throw would be invisible) and asserted by the test.</summary>
    private sealed class FakeOhgHostFacade : IOhgHostFacade
    {
        /// <summary>Route ids the fake "scene" owns, deliberately in a SCRAMBLED order: an
        /// adapter that addressed routes positionally would move the wrong guest.</summary>
        public List<string> KnownRoutes { get; } = new() { "ohg-box-2", "ohg-host", "ohg-box-1" };

        public HashSet<string> Scenes { get; } = new() { "look-scene", "solo-scene", "as-scene", "black-scene", "gallery-scene" };

        public HashSet<int> AssignableSlots { get; } = new() { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };

        public HashSet<string> Transitions { get; } = new() { "cut", "fade", "dip", "wipe" };

        public bool ShadowModeForbidsCalls { get; set; }

        public List<string> Violations { get; } = new();

        public List<(int Slot, string? ParticipantId)> Assignments { get; } = new();

        public List<(int Slot, string Name)> DisplayNames { get; } = new();

        public List<(int Slot, string Title)> LowerThirdTitles { get; } = new();

        public List<(string SceneId, IReadOnlyDictionary<string, int?> Routes)> Cues { get; } = new();

        public Dictionary<string, int?> RouteSlots { get; } = new();

        public List<string> Takes { get; } = new();

        public List<string> Captions { get; } = new();

        public List<string> StatusLines { get; } = new();

        public bool CanTakeValue { get; set; } = true;

        public bool TakeResult { get; set; } = true;

        public int ShowInputCount => 10;

        private void Guard(string member)
        {
            if (ShadowModeForbidsCalls)
            {
                Violations.Add(member);
            }
        }

        public bool AssignZoomParticipant(int slot, string? participantId)
        {
            Guard(nameof(AssignZoomParticipant));
            if (slot < 1 || slot > ShowInputCount)
            {
                return false;
            }

            Assignments.Add((slot, participantId));
            return true;
        }

        public bool SetInputDisplayName(int slot, string name)
        {
            Guard(nameof(SetInputDisplayName));
            if (!AssignableSlots.Contains(slot))
            {
                return false;
            }

            DisplayNames.Add((slot, name));
            return true;
        }

        public bool SetInputLowerThirdTitle(int slot, string title)
        {
            Guard(nameof(SetInputLowerThirdTitle));
            if (!AssignableSlots.Contains(slot))
            {
                return false;
            }

            LowerThirdTitles.Add((slot, title));
            return true;
        }

        public bool SceneExists(string sceneId)
        {
            Guard(nameof(SceneExists));
            return Scenes.Contains(sceneId);
        }

        public IReadOnlyList<string> CueSceneWithRoutes(string sceneId, IReadOnlyDictionary<string, int?> routeSlots)
        {
            Guard(nameof(CueSceneWithRoutes));
            Cues.Add((sceneId, routeSlots));

            var missing = new List<string>();

            // Walk the fake's OWN scrambled route list, exactly as a scene's layer list would be
            // ordered — the adapter must have addressed each route by id.
            foreach (var pair in routeSlots)
            {
                if (KnownRoutes.Contains(pair.Key))
                {
                    RouteSlots[pair.Key] = pair.Value;
                }
                else
                {
                    missing.Add(pair.Key);
                }
            }

            return missing;
        }

        public bool CanTake
        {
            get
            {
                Guard(nameof(CanTake));
                return CanTakeValue;
            }
        }

        public Task<bool> TakeAsync(string transition)
        {
            Guard(nameof(TakeAsync));
            Takes.Add(transition);
            return Task.FromResult(TakeResult);
        }

        public bool IsKnownTransition(string transition)
        {
            Guard(nameof(IsKnownTransition));
            return Transitions.Contains(transition);
        }

        public void SetCaption(string text)
        {
            Guard(nameof(SetCaption));
            Captions.Add(text);
        }

        public void ReportStatus(string line)
        {
            // Deliberately NOT guarded: status lines are operator-visible text, not show state,
            // and are allowed in shadow mode.
            StatusLines.Add(line);
        }
    }

    private static ShowShellConfig Shell(bool driveHost = true, string defaultTransition = "cut",
        string? solo = "solo-scene", string? activeSpeaker = "as-scene", string? black = "black-scene",
        string? gallery = "gallery-scene")
        => new()
        {
            DriveHost = driveHost,
            DefaultTransition = defaultTransition,
            Presets = new ShowPresetScenes
            {
                Solo = solo,
                ActiveSpeaker = activeSpeaker,
                Black = black,
                Gallery = gallery
            }
        };

    private static ShowEngineHostCommand Cmd(string name, string argsJson, long seq = 1)
        => new(1, seq, name, JsonDocument.Parse(argsJson).RootElement.Clone());

    private static (OhgHostAdapter Adapter, FakeOhgHostFacade Facade, List<string> Log) Build(
        ShowShellConfig? shell = null,
        FakeOhgHostFacade? facade = null,
        IReadOnlyDictionary<string, string>? lookPresets = null)
    {
        var f = facade ?? new FakeOhgHostFacade();
        var log = new List<string>();
        var adapter = new OhgHostAdapter(f, shell ?? Shell(), log.Add, lookPresets);
        return (adapter, f, log);
    }

    /// <summary>The exact <c>applyLook</c> arg shape: <c>boxes</c> is a Map, and
    /// <c>protocol.ts</c>'s replacer puts a Map on the wire as a [key, value] pair array.</summary>
    private static string LookArgs(string lookId, string scenePreset, string boxesPairs,
        string hostSlot = "null", string readerSlot = "null")
        => $$"""[{"lookId":"{{lookId}}","scenePreset":"{{scenePreset}}","hostSlot":{{hostSlot}},"readerSlot":{{readerSlot}},"boxes":{{boxesPairs}}}]""";

    // ---- spec §8, row by row --------------------------------------------------------

    [Fact]
    public async Task AssignSlot_AssignsTheParticipantToTheSlot()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("assignSlot", """[3,"guest-77"]"""));

        Assert.Null(refusal);
        Assert.Equal(new[] { (3, (string?)"guest-77") }, facade.Assignments);
    }

    [Fact]
    public async Task AssignSlot_NullParticipant_ClearsTheSlot()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("assignSlot", "[4,null]"));

        Assert.Null(refusal);
        Assert.Equal(new[] { (4, (string?)null) }, facade.Assignments);
    }

    [Fact]
    public async Task AssignSlot_OutOfRange_Refuses()
    {
        var (adapter, facade, log) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("assignSlot", """[11,"guest-77"]"""));

        Assert.Equal("assignSlot: slot 11 is outside 1..10", refusal);
        Assert.Empty(facade.Assignments);
        Assert.Contains("assignSlot: slot 11 is outside 1..10", log);
    }

    [Fact]
    public async Task ApplyLook_CuesThePresetSceneAndRewritesRoutesById()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("applyLook",
            LookArgs("look-a", "look-scene", "[[1,4],[2,7]]", hostSlot: "1", readerSlot: "2")));

        Assert.Null(refusal);
        var cue = Assert.Single(facade.Cues);
        Assert.Equal("look-scene", cue.SceneId);
        Assert.Equal(4, facade.RouteSlots["ohg-box-1"]);
        Assert.Equal(7, facade.RouteSlots["ohg-box-2"]);
        Assert.Equal(1, facade.RouteSlots["ohg-host"]);
    }

    [Fact]
    public async Task ApplyLook_NullReaderSlot_NeverTouchesTheReaderRoute()
    {
        var (adapter, facade, _) = Build();
        facade.KnownRoutes.Add("ohg-reader");

        await adapter.ApplyAsync(Cmd("applyLook",
            LookArgs("look-a", "look-scene", "[[1,4]]", hostSlot: "1", readerSlot: "null")));

        var cue = Assert.Single(facade.Cues);
        Assert.False(cue.Routes.ContainsKey("ohg-reader"));
        Assert.False(facade.RouteSlots.ContainsKey("ohg-reader"));
    }

    [Fact]
    public async Task ApplyLook_NullBoxSlot_IsCarriedAsANullRouteSlot()
    {
        var (adapter, facade, _) = Build();

        await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "look-scene", "[[1,null],[2,7]]")));

        Assert.Null(facade.RouteSlots["ohg-box-1"]);
        Assert.Equal(7, facade.RouteSlots["ohg-box-2"]);
    }

    [Fact]
    public async Task ApplyLook_MissingPresetScene_RefusesAndAppliesNothing()
    {
        var (adapter, facade, log) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "ghost-scene", "[[1,4]]")));

        Assert.Equal("applyLook: preset scene 'ghost-scene' does not exist", refusal);
        Assert.Empty(facade.Cues);
        Assert.Empty(facade.RouteSlots);
        Assert.Contains(refusal, log);
    }

    [Fact]
    public async Task SetPreview_Look_CuesTheLooksRememberedPreset()
    {
        var (adapter, facade, _) = Build();
        await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "look-scene", "[[1,4]]")));
        facade.Cues.Clear();

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"look","lookId":"look-a"}]"""));

        Assert.Null(refusal);
        Assert.Equal("look-scene", Assert.Single(facade.Cues).SceneId);
    }

    [Fact]
    public async Task SetPreview_LookWithNoConfiguredPresetAndNoApplyLook_Refuses()
    {
        var (adapter, facade, log) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"look","lookId":"look-z"}]"""));

        Assert.Equal("setPreview: look 'look-z' has no configured scene preset", refusal);
        Assert.Empty(facade.Cues);
        Assert.Contains(refusal, log);
    }

    /// <summary>
    /// THE FIRST CUE OF A LOOK MUST WORK (Task 13, fix round 1). The engine emits
    /// <c>setPreview({kind:"look"})</c> BEFORE the <c>applyLook</c> that first names that look's
    /// scene preset — same tick, one seq apart — which the cross-process adapter conformance run
    /// caught and neither side's own tests could see. An adapter seeded from
    /// <c>config.engine.looks[]</c> answers it; one that learns only from <c>applyLook</c> refuses
    /// the first cue of every look on every show.
    /// </summary>
    [Fact]
    public async Task SetPreview_ConfiguredButNeverAppliedLook_CuesItsPresetWithoutRefusing()
    {
        var (adapter, facade, log) = Build(
            lookPresets: new Dictionary<string, string>(StringComparer.Ordinal) { ["look-a"] = "look-scene" });

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"look","lookId":"look-a"}]"""));

        Assert.Null(refusal);
        Assert.Empty(log);
        Assert.Equal("look-scene", Assert.Single(facade.Cues).SceneId);
    }

    /// <summary>A later <c>applyLook</c> is still the live authority: a look re-pointed at another
    /// scene engine-side cues the NEW scene, not the one the config was seeded with.</summary>
    [Fact]
    public async Task ApplyLook_OverridesTheConfigSeededPresetForLaterPreviews()
    {
        var (adapter, facade, _) = Build(
            lookPresets: new Dictionary<string, string>(StringComparer.Ordinal) { ["look-a"] = "look-scene" });

        await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "solo-scene", "[[1,4]]")));
        facade.Cues.Clear();

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"look","lookId":"look-a"}]"""));

        Assert.Null(refusal);
        Assert.Equal("solo-scene", Assert.Single(facade.Cues).SceneId);
    }

    [Fact]
    public async Task SetPreview_Slot_CuesSoloWithBoxOneOnThatSlot()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"slot","slot":6}]"""));

        Assert.Null(refusal);
        Assert.Equal("solo-scene", Assert.Single(facade.Cues).SceneId);
        Assert.Equal(6, facade.RouteSlots["ohg-box-1"]);
    }

    [Fact]
    public async Task SetPreview_SlotBelowOne_Refuses()
    {
        var (adapter, facade, _) = Build();

        Assert.Equal("setPreview: slot must be >= 1",
            await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"slot","slot":0}]""")));
        Assert.Equal("setPreview: slot must be >= 1",
            await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"slot","slot":-1}]""")));
        Assert.Empty(facade.Cues);
    }

    [Fact]
    public async Task SetPreview_NoSoloPresetConfigured_Refuses()
    {
        var (adapter, facade, _) = Build(Shell(solo: null));

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"slot","slot":2}]"""));

        Assert.Equal("setPreview: no solo preset configured", refusal);
        Assert.Empty(facade.Cues);
    }

    [Theory]
    [InlineData("activeSpeaker", "as-scene")]
    [InlineData("black", "black-scene")]
    [InlineData("gallery", "gallery-scene")]
    public async Task SetPreview_FixedPresets_CueTheConfiguredScene(string kind, string sceneId)
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("setPreview", $$"""[{"kind":"{{kind}}"}]"""));

        Assert.Null(refusal);
        Assert.Equal(sceneId, Assert.Single(facade.Cues).SceneId);
    }

    [Fact]
    public async Task SetPreview_UnconfiguredFixedPreset_Refuses()
    {
        var (adapter, _, _) = Build(Shell(gallery: null));

        Assert.Equal("setPreview: no gallery preset configured",
            await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"gallery"}]""")));
    }

    [Fact]
    public async Task SetPreview_ConfiguredPresetSceneMissing_Refuses()
    {
        var (adapter, facade, _) = Build();
        facade.Scenes.Remove("black-scene");

        Assert.Equal("setPreview: preset scene 'black-scene' does not exist",
            await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"black"}]""")));
    }

    [Fact]
    public async Task Cut_TakesWithTheCutTransition()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("cut", "[]"));

        Assert.Null(refusal);
        Assert.Equal("cut", Assert.Single(facade.Takes));
    }

    [Fact]
    public async Task Cut_TakeUnavailable_Refuses()
    {
        var (adapter, facade, log) = Build();
        facade.CanTakeValue = false;

        var refusal = await adapter.ApplyAsync(Cmd("cut", "[]"));

        Assert.Equal("cut: take unavailable", refusal);
        Assert.Empty(facade.Takes);
        Assert.Contains(refusal, log);
    }

    [Fact]
    public async Task Cut_TakeRefusedByTheShell_Refuses()
    {
        var (adapter, facade, _) = Build();
        facade.TakeResult = false;

        Assert.Equal("cut: take refused by the shell", await adapter.ApplyAsync(Cmd("cut", "[]")));
    }

    [Fact]
    public async Task Auto_NullTransition_UsesTheConfiguredDefault()
    {
        var (adapter, facade, _) = Build(Shell(defaultTransition: "fade"));

        var refusal = await adapter.ApplyAsync(Cmd("auto", "[null]"));

        Assert.Null(refusal);
        Assert.Equal("fade", Assert.Single(facade.Takes));
    }

    [Fact]
    public async Task Auto_NamedTransition_TakesWithIt()
    {
        var (adapter, facade, _) = Build();

        Assert.Null(await adapter.ApplyAsync(Cmd("auto", """["wipe"]""")));
        Assert.Equal("wipe", Assert.Single(facade.Takes));
    }

    [Fact]
    public async Task Auto_UnknownTransition_Refuses()
    {
        var (adapter, facade, log) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("auto", """["swirl"]"""));

        Assert.Equal("auto: unknown transition 'swirl'", refusal);
        Assert.Empty(facade.Takes);
        Assert.Contains(refusal, log);
    }

    [Fact]
    public async Task SetNameplates_SetsDisplayNameAndLowerThirdTitlePerPlate()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("setNameplates", """
            [[{"position":{"kind":"host"},"slot":1,"name":"Ada","location":"Helsinki","tone":"host"},
              {"position":{"kind":"box","box":2},"slot":5,"name":"Bo","location":"Oslo","tone":"guest"}]]
            """));

        Assert.Null(refusal);
        Assert.Equal(new[] { (1, "Ada"), (5, "Bo") }, facade.DisplayNames);
        Assert.Equal(new[] { (1, "Helsinki"), (5, "Oslo") }, facade.LowerThirdTitles);
    }

    [Fact]
    public async Task SetNameplates_UnassignedSlot_RefusesNamingTheSlot()
    {
        var (adapter, facade, log) = Build();
        facade.AssignableSlots.Remove(5);

        var refusal = await adapter.ApplyAsync(Cmd("setNameplates", """
            [[{"position":{"kind":"box","box":2},"slot":5,"name":"Bo","location":"Oslo","tone":"guest"}]]
            """));

        Assert.Equal("setNameplates: slot 5 is not assigned", refusal);
        Assert.Contains(refusal, log);
    }

    [Fact]
    public async Task SetQuestion_SetsTheCaptionToTheQuestionText()
    {
        var (adapter, facade, _) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("setQuestion",
            """[{"askerName":"Ada","text":"Why now?","tag":"policy","votes":4}]"""));

        Assert.Null(refusal);
        Assert.Equal("Why now?", Assert.Single(facade.Captions));
    }

    [Fact]
    public async Task SetQuestion_Null_ClearsTheCaption()
    {
        var (adapter, facade, _) = Build();

        Assert.Null(await adapter.ApplyAsync(Cmd("setQuestion", "[null]")));
        Assert.Equal("", Assert.Single(facade.Captions));
    }

    [Fact]
    public async Task SetGallery_ReportsTheUnappliedNoteOncePerAdapterLifetime()
    {
        var (adapter, facade, log) = Build();

        Assert.Null(await adapter.ApplyAsync(Cmd("setGallery", "[[[1,3],[2,4]]]", seq: 1)));
        Assert.Null(await adapter.ApplyAsync(Cmd("setGallery", "[[[1,4],[2,3]]]", seq: 2)));

        var note = Assert.Single(facade.StatusLines);
        Assert.Equal("gallery: cell order not applied (Tiles has no explicit order API)", note);
        Assert.Contains(note, log);
        Assert.Empty(facade.Cues);
    }

    [Fact]
    public async Task UnknownCommand_Refuses()
    {
        var (adapter, _, log) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("teleport", "[]"));

        Assert.Equal("unknown host command 'teleport'", refusal);
        Assert.Contains(refusal, log);
    }

    [Fact]
    public async Task MalformedArgs_RefuseInsteadOfThrowing()
    {
        var (adapter, facade, log) = Build();

        var refusal = await adapter.ApplyAsync(Cmd("assignSlot", """["three","guest"]"""));

        Assert.NotNull(refusal);
        Assert.StartsWith("assignSlot: malformed args: ", refusal);
        Assert.Empty(facade.Assignments);
        Assert.Contains(refusal!, log);

        Assert.StartsWith("applyLook: malformed args: ", await adapter.ApplyAsync(Cmd("applyLook", "[]")));
        Assert.StartsWith("setPreview: malformed args: ", await adapter.ApplyAsync(Cmd("setPreview", """[{}]""")));
        Assert.StartsWith("auto: malformed args: ", await adapter.ApplyAsync(Cmd("auto", "[7]")));
    }

    // ---- the four named extras ------------------------------------------------------

    [Fact]
    public async Task RouteIdNaming_AReorderedLayerDoesNotMoveAGuest()
    {
        var (adapter, facade, _) = Build();

        // boxes arrive in DESCENDING box order (a Map's insertion order is whatever the engine
        // built, and the fake's own route list is scrambled too) — box 2 first, box 1 second.
        await adapter.ApplyAsync(Cmd("applyLook",
            LookArgs("look-a", "look-scene", "[[2,7],[1,4]]", hostSlot: "1")));

        Assert.Equal(4, facade.RouteSlots["ohg-box-1"]);
        Assert.Equal(7, facade.RouteSlots["ohg-box-2"]);
        Assert.Equal(1, facade.RouteSlots["ohg-host"]);
    }

    [Fact]
    public async Task MissingRoute_IsReportedOncePerPresetAndRouteSet()
    {
        var (adapter, facade, log) = Build();
        facade.KnownRoutes.Remove("ohg-box-2");

        // Same preset, same missing-route set, twice ⇒ reported once.
        Assert.Null(await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "look-scene", "[[1,4],[2,7]]"), seq: 1)));
        Assert.Null(await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "look-scene", "[[1,5],[2,8]]"), seq: 2)));

        var note = Assert.Single(facade.StatusLines);
        Assert.Equal("look 'look-a': preset 'look-scene' has no route(s) ohg-box-2", note);
        Assert.Contains(note, log);

        // The rest still applied.
        Assert.Equal(5, facade.RouteSlots["ohg-box-1"]);

        // A DIFFERENT missing set on the same preset is a new report.
        facade.KnownRoutes.Remove("ohg-host");
        await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "look-scene", "[[1,4],[2,7]]", hostSlot: "3"), seq: 3));
        Assert.Equal(2, facade.StatusLines.Count);
        Assert.Equal("look 'look-a': preset 'look-scene' has no route(s) ohg-box-2, ohg-host", facade.StatusLines[1]);
    }

    [Fact]
    public async Task ShadowMode_RecordsEverything_AndTouchesNothing()
    {
        var facade = new FakeOhgHostFacade { ShadowModeForbidsCalls = true };
        var (adapter, _, _) = Build(Shell(driveHost: false), facade);

        Assert.False(adapter.DriveHost);

        Assert.Null(await adapter.ApplyAsync(Cmd("assignSlot", """[3,"guest-77"]""", seq: 1)));
        Assert.Null(await adapter.ApplyAsync(Cmd("applyLook", LookArgs("look-a", "look-scene", "[[1,4]]"), seq: 2)));
        Assert.Null(await adapter.ApplyAsync(Cmd("setPreview", """[{"kind":"black"}]""", seq: 3)));
        Assert.Null(await adapter.ApplyAsync(Cmd("cut", "[]", seq: 4)));
        Assert.Null(await adapter.ApplyAsync(Cmd("auto", "[null]", seq: 5)));
        Assert.Null(await adapter.ApplyAsync(Cmd("setGallery", "[[[1,3]]]", seq: 6)));
        Assert.Null(await adapter.ApplyAsync(Cmd("setNameplates",
            """[[{"position":{"kind":"host"},"slot":1,"name":"Ada","location":"Helsinki","tone":"host"}]]""", seq: 7)));
        Assert.Null(await adapter.ApplyAsync(Cmd("setQuestion", "[null]", seq: 8)));

        Assert.Empty(facade.Violations);
        Assert.Equal(8, adapter.ShadowLog.Count);
        Assert.Equal("""1 assignSlot([3,"guest-77"])""", adapter.ShadowLog[0]);
        Assert.Equal("8 setQuestion([null])", adapter.ShadowLog[7]);
        Assert.Equal("8 setQuestion([null])", adapter.ShadowLastCommand);

        // ReportStatus IS allowed in shadow mode — the gallery note is operator-visible text.
        Assert.Equal("gallery: cell order not applied (Tiles has no explicit order API)",
            Assert.Single(facade.StatusLines));
    }

    [Fact]
    public async Task SetGallery_IsRecordedEvenWhenDriving()
    {
        var (adapter, _, _) = Build(Shell(driveHost: true));

        Assert.True(adapter.DriveHost);
        Assert.Null(await adapter.ApplyAsync(Cmd("assignSlot", """[1,"g"]""", seq: 1)));
        Assert.Null(await adapter.ApplyAsync(Cmd("setGallery", "[[[1,3],[2,4]]]", seq: 2)));

        var recorded = Assert.Single(adapter.ShadowLog);
        Assert.Equal("2 setGallery([[[1,3],[2,4]]])", recorded);
        Assert.Equal(recorded, adapter.ShadowLastCommand);
    }

    // ---- shadow-log mechanics -------------------------------------------------------

    [Fact]
    public void FormatForShadow_IsSeqNameAndTheRawArgsJson()
    {
        Assert.Equal("""17 assignSlot([3,"guest-77"])""",
            OhgHostAdapter.FormatForShadow(Cmd("assignSlot", """[3,"guest-77"]""", seq: 17)));
    }

    [Fact]
    public async Task ShadowLog_IsBoundedToTheFiftyNewest()
    {
        var (adapter, _, _) = Build(Shell(driveHost: false));

        for (var seq = 1; seq <= 60; seq++)
        {
            await adapter.ApplyAsync(Cmd("cut", "[]", seq));
        }

        Assert.Equal(50, adapter.ShadowLog.Count);
        Assert.Equal("11 cut([])", adapter.ShadowLog[0]);
        Assert.Equal("60 cut([])", adapter.ShadowLog[49]);
        Assert.Equal("60 cut([])", adapter.ShadowLastCommand);
    }

    [Fact]
    public void RoutesForLook_NamesEveryBoxAndOnlyTheChairsThatAreSeated()
    {
        var placement = JsonDocument.Parse(
            """{"lookId":"l","scenePreset":"s","hostSlot":2,"readerSlot":null,"boxes":[[3,9],[1,null]]}""")
            .RootElement.Clone();

        var routes = OhgHostAdapter.RoutesForLook(placement);

        Assert.Equal(3, routes.Count);
        Assert.Equal(9, routes["ohg-box-3"]);
        Assert.Null(routes["ohg-box-1"]);
        Assert.Equal(2, routes["ohg-host"]);
        Assert.False(routes.ContainsKey("ohg-reader"));
    }
}
