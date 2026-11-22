// Settings quick wins (docs/superpowers/plans/2026-11-22-mac-settings-quickwins.md):
// A1, A5, Z1, Z2, O1 and the Companion autoassign action. Own file so the
// parallel lanes that extend ShellTests.swift do not collide line-for-line.
//
// Each test names, in its doc comment, the mutation it must turn red
// (plan-authoring rule 7). Fixture invariants are stated where they matter.

import Foundation

extension ShellTests {

    @MainActor
    static var quickWinCases: [(String, () -> Void)] {
        [
            ("quickwins/a1-program-mix-topology", testA1ProgramMixTopology),
            ("quickwins/a1-per-guest-doubling-guard", testA1PerGuestIsoDoublingGuard),
            ("quickwins/a1-flip-back-and-no-meeting-mix", testA1FlipBackAndNoMeetingMix),
            ("quickwins/a1-preference-parse-format", testA1PreferenceParseAndFormat),
            ("quickwins/a1-model-flip-pushes-persists-reasserts", testA1ModelFlipPushesPersistsAndReasserts),
            ("quickwins/a1-control-action-and-state", testA1ControlActionAndState),
        ]
    }

    // ── A1: Zoom audio mode ─────────────────────────────────────────────────
    // Fixture invariant for the policy tests: stems "11" and "22"; the default
    // bus set is exactly AudioConsolePolicy.defaultSendBuses (5 buses).

    private static let a1Buses = ["master", "mon", "pgm-l", "pgm-r", "stream"]

    private static func a1Buses(_ sends: [JSONObject], _ source: String) -> [String] {
        sends.filter { $0["sourceId"] as? String == source }.compactMap { $0["busId"] as? String }.sorted()
    }

    /// Port of Windows `RoutesTheMeetingMixNotIsoParticipants`: ProgramMix gives
    /// zoom-mix the five defaults at 0 dB, an operator's own stem crosspoint
    /// survives, and no other stem send appears (exactly 6 sends).
    /// Mutation: seed stems in ProgramMix -> red on the count and on "22".
    private static func testA1ProgramMixTopology() {
        let sends = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["11", "22"], captureAudioIds: [], mediaRouted: false,
            operatorEdits: [AudioRouteKey(source: "11", bus: "master"): .init(enabled: true, gainDb: -6)],
            zoomAudioMode: .programMix, meetingAudio: true)
        expectEqual(a1Buses(sends, "zoom-mix"), a1Buses, "zoom-mix carries the program defaults")
        expect(sends.filter { $0["sourceId"] as? String == "zoom-mix" }.allSatisfy { ($0["gainDb"] as? Double) == 0 },
               "zoom-mix defaults are unity")
        expectEqual(a1Buses(sends, "11"), ["master"], "the operator's deliberate stem route survives, nothing else")
        expectEqual(sends.first { $0["sourceId"] as? String == "11" }?["gainDb"] as? Double, -6,
                    "at the operator's gain")
        expectEqual(a1Buses(sends, "22"), [], "no other stem is routed")
        expectEqual(sends.count, 6, "five zoom-mix sends + the one operator stem send")
    }

    /// Port of `PerGuestIsoRoutesStemsAndDropsTheMixFromProgram`: an operator
    /// zoom-mix -> master edit is DROPPED (the doubling guard runs after edits),
    /// a zoom-mix send to a NON-default bus survives, both stems get 5 defaults,
    /// and an operator's custom stem gain survives.
    /// Mutation: remove the guard -> red on "zoom-mix master dropped".
    private static func testA1PerGuestIsoDoublingGuard() {
        let sends = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["11", "22"], captureAudioIds: [], mediaRouted: false,
            operatorEdits: [
                AudioRouteKey(source: "zoom-mix", bus: "master"): .init(enabled: true, gainDb: 0),
                AudioRouteKey(source: "zoom-mix", bus: "iso-1"): .init(enabled: true, gainDb: -3),
                AudioRouteKey(source: "11", bus: "master"): .init(enabled: true, gainDb: -6),
            ],
            zoomAudioMode: .perGuestIso, meetingAudio: true)
        expectEqual(a1Buses(sends, "zoom-mix"), ["iso-1"],
                    "zoom-mix master dropped (doubled voices); its iso-1 send is not a default bus and stays")
        expectEqual(a1Buses(sends, "11"), a1Buses, "stem 11 completes onto every default bus")
        expectEqual(a1Buses(sends, "22"), a1Buses, "stem 22 likewise")
        expectEqual(sends.first { $0["sourceId"] as? String == "11" && $0["busId"] as? String == "master" }?["gainDb"]
                        as? Double, -6, "the operator's custom stem gain survives")
    }

    /// Port of `FlippingBackToProgramMixRestoresTheZ1Shape` plus the Mac-only
    /// gate: with no meeting mix subscribed, ProgramMix routes NO zoom-mix
    /// (no strip exists, and the FADER LAW would drop it).
    /// Mutation: seed zoom-mix regardless of meetingAudio -> red.
    private static func testA1FlipBackAndNoMeetingMix() {
        let back = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["11"], captureAudioIds: [], mediaRouted: false, operatorEdits: [:],
            zoomAudioMode: .programMix, meetingAudio: true)
        expectEqual(a1Buses(back, "11"), [], "flip-back: stems stop being generated")
        expectEqual(a1Buses(back, "zoom-mix"), a1Buses, "flip-back: zoom-mix completes onto every bus")
        let noMix = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["11"], captureAudioIds: ["capture:cam"], mediaRouted: false, operatorEdits: [:],
            zoomAudioMode: .programMix, meetingAudio: false)
        expectEqual(a1Buses(noMix, "zoom-mix"), [], "no meeting mix subscribed -> no zoom-mix route")
        expectEqual(a1Buses(noMix, "capture:cam"), a1Buses, "capture defaults are unaffected by the mode")
    }

    /// The five ZoomAudioModePreferenceTests, verbatim cases.
    /// Mutation: parse defaulting to programMix -> red on the fallbacks.
    private static func testA1PreferenceParseAndFormat() {
        expectEqual(ZoomAudioMode.parse("programMix"), .programMix, "reads programMix")
        expectEqual(ZoomAudioMode.parse("perGuestIso"), .perGuestIso, "reads perGuestIso")
        expectEqual(ZoomAudioMode.parse("PROGRAMMIX"), .programMix, "case-insensitive")
        expectEqual(ZoomAudioMode.parse("  programMix  "), .programMix, "trimmed")
        for junk in [nil, "", "   ", "chaos"] as [String?] {
            expectEqual(ZoomAudioMode.parse(junk), .perGuestIso, "'\(junk ?? "nil")' falls back to perGuestIso")
        }
        expectEqual(ZoomAudioMode.programMix.persisted, "programMix", "documented wire string")
        expectEqual(ZoomAudioMode.perGuestIso.persisted, "perGuestIso", "documented wire string")
        for mode in [ZoomAudioMode.programMix, .perGuestIso] {
            expectEqual(ZoomAudioMode.parse(mode.persisted), mode, "round trip")
        }
    }

    /// The model path: the setter pushes ONE routing base with the new shape,
    /// says the Windows sentence, persists, restores, and a core restart's
    /// reassert batch carries the ProgramMix matrix.
    /// Mutation: drop pushRoutingBase() from setZoomAudioMode -> red (observer empty).
    @MainActor
    private static func testA1ModelFlipPushesPersistsAndReasserts() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        model.meetingAudioSubscribed = true
        var sent: [JSONObject] = []
        model.routingBaseSendObserver = { sent.append($0) }
        model.setZoomAudioMode(.programMix)
        spin(until: { !sent.isEmpty }, limit: 2)
        let sends = (sent.last?["commands"] as? [JSONObject])?.first?["sends"] as? [JSONObject] ?? []
        expectEqual(a1Buses(sends, "zoom-mix"), a1Buses, "the pushed matrix routes zoom-mix")
        expectEqual(a1Buses(sends, "11"), [], "and no stem")
        expectEqual(model.takeStatus, ZoomAudioMode.programMix.statusSentence, "the Windows status sentence")
        expectEqual(model.currentPrefs().zoomAudioMode, "programMix", "persisted as intent")

        let relaunched = AppModel()
        relaunched.applyRestoredPrefs(model.currentPrefs())
        expectEqual(relaunched.zoomAudioMode, .programMix, "a relaunch restores the mode")

        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.reassertAudioConsole()
        let routing = batches.last?.first { $0["type"] as? String == "sync-audio-routing-matrix" }
        expectEqual(a1Buses(routing?["sends"] as? [JSONObject] ?? [], "zoom-mix"), a1Buses,
                    "a respawned core gets the ProgramMix matrix back")
    }

    /// audio.zoomMode.set through the real surface: applies, refuses junk
    /// without a change, and the state reports the mode.
    /// Mutation: drop the `case` -> red (dispatch falls to Unknown action).
    @MainActor
    private static func testA1ControlActionAndState() {
        let model = AppModel()
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(surface, "audio.zoomMode.set", ["ProgramMix"]), .success, "programMix accepted (any case)")
        expectEqual(model.zoomAudioMode, .programMix, "applied")
        expectEqual(invoke(surface, "audio.zoomMode.set", ["chaos"]),
                    .fail("mode must be 'perGuestIso' or 'programMix'."), "junk refused with the Windows text")
        expectEqual(model.zoomAudioMode, .programMix, "a refusal changes nothing")
        expectEqual(AppModelControlSurface.state(model).dictionary["zoomAudioMode"] as? String, "programMix",
                    "state reports the mode")
        expectEqual(invoke(surface, "audio.zoomMode.set", [" perGuestIso "]), .success, "trimmed")
        expectEqual(model.zoomAudioMode, .perGuestIso, "back to per-guest")
    }
}
