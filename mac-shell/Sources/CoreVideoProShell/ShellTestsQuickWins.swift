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
            ("quickwins/a5-full-state-entry", testA5FullStateEntry),
            ("quickwins/a5-console-source-and-routing", testA5ConsoleSourceAndRouting),
            ("quickwins/a5-status-from-the-core", testA5StatusComesFromTheCore),
            ("quickwins/a5-prefs-and-unplugged-device", testA5PrefsAndUnpluggedDevice),
            ("quickwins/z1-windows-preference-table", testZ1WindowsPreferenceTable),
            ("quickwins/z1-join-carries-and-latches-the-ceiling", testZ1JoinCarriesAndLatchesTheCeiling),
            ("quickwins/z1-prefs-round-trip", testZ1PrefsRoundTrip),
            ("quickwins/z2-clamp-and-label", testZ2ClampAndLabel),
            ("quickwins/z2-both-copies-reach-the-core", testZ2BothCopiesReachTheCore),
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

    // ── A5: local audio source ──────────────────────────────────────────────
    // Fixture invariant: the device list holds exactly "mic-uid" ("USB Mic");
    // "gone-uid" is never in it (an unplugged saved device).

    private static let a5Mic = AudioInputDevice(id: "mic-uid", name: "USB Mic")

    private static func a5Local(_ command: JSONObject) -> JSONObject? {
        (command["sources"] as? [JSONObject])?.first { $0["captureDeviceId"] as? String == "local-machine-audio" }
    }

    /// The full-state sync-capture-audio-sources carries the local entry only
    /// while enabled AND connected, and the core-restart reassert carries it.
    /// Mutation: ignore `enabled` in LocalAudioPolicy.entry -> red ("disabled -> absent").
    @MainActor
    private static func testA5FullStateEntry() {
        let model = AppModel()
        model.audioInputs = [a5Mic]
        model.localAudio = LocalAudioSetting(enabled: true, deviceId: "mic-uid", deviceName: "USB Mic")
        let entry = a5Local(model.captureAudioSourcesCommand())
        expectEqual(entry?["audioDeviceId"] as? String, "mic-uid", "enabled + connected -> the entry")
        expectEqual(entry?["audioSourceKind"] as? String, "coreaudio-input", "CoreAudio input kind")
        expectEqual(entry?["nativeAudioDeviceId"] as? String, "mic-uid", "native id = the UID")
        expectEqual(entry?["audioDeviceName"] as? String, "USB Mic", "device name")
        model.localAudio.enabled = false
        expect(a5Local(model.captureAudioSourcesCommand()) == nil, "disabled -> absent")
        model.localAudio = LocalAudioSetting(enabled: true, deviceId: "gone-uid", deviceName: "Old Mic")
        expect(a5Local(model.captureAudioSourcesCommand()) == nil, "enabled but unplugged -> absent")
        model.localAudio = LocalAudioSetting(enabled: true, deviceId: "mic-uid", deviceName: "USB Mic")
        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.reassertAudioConsole()
        let sources = batches.last?.first { $0["type"] as? String == "sync-capture-audio-sources" }
        expect(sources.flatMap(a5Local) != nil, "a respawned core gets the local source back")
    }

    /// A configured local source is a console source: a strip and the five
    /// default sends; unconfigured, neither.
    /// Mutation: leave it out of consoleCaptureAudioIds -> red on both.
    @MainActor
    private static func testA5ConsoleSourceAndRouting() {
        let model = AppModel()
        model.audioInputs = [a5Mic]
        model.localAudio = LocalAudioSetting(enabled: true, deviceId: "mic-uid", deviceName: "USB Mic")
        let sends = model.routingBaseCommand()?["sends"] as? [JSONObject] ?? []
        expectEqual(a1Buses(sends, "local-machine-audio"), a1Buses, "five default sends")
        expect(model.expectedAudioSourceIds.contains("local-machine-audio"), "it has a strip")
        expectEqual(model.audioSourceLabel("local-machine-audio"), "Local machine audio", "strip label")
        model.localAudio.enabled = false
        let off = model.routingBaseCommand()?["sends"] as? [JSONObject] ?? []
        expectEqual(a1Buses(off, "local-machine-audio"), [], "off -> no sends")
        expect(!model.expectedAudioSourceIds.contains("local-machine-audio"), "off -> no strip")
    }

    /// Windows ResolveLocalAudioOperatorStatus over a fixture core row, every
    /// branch. Mutation: swap the signalPresent / captureStreaming checks -> red.
    private static func testA5StatusComesFromTheCore() {
        let on = LocalAudioSetting(enabled: true, deviceId: "mic-uid", deviceName: "USB Mic")
        func status(_ setting: LocalAudioSetting, _ rows: [JSONObject]) -> String {
            LocalAudioPolicy.status(setting, devices: [a5Mic], captureAudioSources: rows)
        }
        func row(streaming: Bool, signal: Bool) -> JSONObject {
            ["captureDeviceId": "local-machine-audio", "captureStreaming": streaming, "signalPresent": signal]
        }
        expectEqual(status(LocalAudioSetting(), []), "Local audio source off", "off")
        expectEqual(status(LocalAudioSetting(enabled: true), []), "Choose an audio input", "no device")
        expectEqual(status(LocalAudioSetting(enabled: true, deviceId: "gone-uid", deviceName: "Old Mic"), []),
                    "Old Mic is not connected", "a saved device that is unplugged is named")
        expectEqual(status(on, [["captureDeviceId": "capture:cam"]]), "Waiting for audio", "no row for it yet")
        expectEqual(status(on, [row(streaming: true, signal: true)]), "Audio detected", "signal")
        expectEqual(status(on, [row(streaming: false, signal: false)]), "Starting audio source", "not streaming")
        expectEqual(status(on, [row(streaming: true, signal: false)]), "No audio detected", "silent")
        // The fourth combination is the only one that tells the branch ORDER apart:
        // Windows checks signalPresent before captureStreaming.
        expectEqual(status(on, [row(streaming: false, signal: true)]), "Audio detected",
                    "signal wins over a not-yet-streaming flag (Windows order)")
        expectEqual(LocalAudioPolicy.captureAudioRows(["captureAudioSources": ["sources": [row(streaming: true, signal: true)]]]).count,
                    1, "reads the core node's sources[]")
    }

    /// Relaunch restores the choice, and a saved device that is not connected
    /// keeps the choice (it is not cleared or replaced).
    /// Mutation: drop the localAudio restore lines -> red.
    @MainActor
    private static func testA5PrefsAndUnpluggedDevice() {
        let model = AppModel()
        model.localAudio = LocalAudioSetting(enabled: true, deviceId: "gone-uid", deviceName: "Old Mic")
        let relaunched = AppModel()
        relaunched.audioInputs = [a5Mic]
        relaunched.applyRestoredPrefs(model.currentPrefs())
        expectEqual(relaunched.localAudio,
                    LocalAudioSetting(enabled: true, deviceId: "gone-uid", deviceName: "Old Mic"),
                    "the saved choice survives a relaunch even while the device is unplugged")
        expectEqual(relaunched.localAudioStatus, "Old Mic is not connected", "and says so")
    }

    // ── Z1: Zoom quality pickers ────────────────────────────────────────────

    /// The Windows ZoomCameraResolutionPreferenceTests table, case for case.
    /// Mutation: map "720p" to tier 2 -> red on the tier row.
    private static func testZ1WindowsPreferenceTable() {
        for (choice, tier) in [("360p", 0), ("720p", 1), ("1080p", 2), ("unknown", 2)] as [(String?, Int)]
            + [(nil, 2)] {
            expectEqual(ZoomCameraResolutionPreference.toSdkTier(choice), tier, "\(choice ?? "nil") -> tier \(tier)")
            expectEqual(ZoomCameraResolutionPreference.fromSdkTier(tier),
                        ZoomCameraResolutionPreference.normalize(choice), "tier \(tier) round trip")
        }
        expect(ZoomCameraResolutionPreference.status(preferred: "720p", applied: "1080p", inMeeting: true)
            .contains("720p pending for next Zoom join"), "mid-meeting edit reads pending")
        expect(ZoomCameraResolutionPreference.status(preferred: "720p", applied: "720p", inMeeting: true)
            .contains("Requested max 720p this meeting"), "applied reads this meeting")
        expect(ZoomCameraResolutionPreference.status(preferred: "360p", applied: "1080p", inMeeting: false)
            .contains("Format shows actual delivery"), "out of a meeting")
        for (choice, w, h) in [("360p", 640, 360), ("720p", 1280, 720), ("1080p", 1920, 1080)] {
            let d = ZoomCameraResolutionPreference.dimensions(choice)
            expectEqual("\(d.width)x\(d.height)", "\(w)x\(h)", "\(choice) dimensions")
        }
        expectEqual(ZoomCameraFrameRatePreference.normalize(13), 60, "an unknown fps is 60")
        expect(ZoomCameraFrameRatePreference.status(preferred: 30, applied: 30, inMeeting: true)
            .contains("Zoom may deliver more"), "fps applied copy")
        expect(ZoomCameraFrameRatePreference.status(preferred: 24, applied: 30, inMeeting: true)
            .contains("pending for next Zoom join"), "fps pending copy")
    }

    /// The join payload carries the tier as a NUMBER and the fps; the join that
    /// reaches the meeting latches them; an edit mid-meeting changes the picker
    /// but not the applied ceiling, and the status says "pending".
    /// Mutations: send the tier as a string -> red (payload); latch at edit time
    /// instead of at the join -> red (pending).
    @MainActor
    private static func testZ1JoinCarriesAndLatchesTheCeiling() {
        let core = FakeTakeCore()
        let model = AppModel()
        model.quitRequestForTesting = { body, _ in
            _ = try await core.handle(body)
            return ["snapshot": ["meetingState": "in_meeting", "warnings": [] as [String]]]
        }
        model.setZoomCameraMaxResolution("720p")
        model.setZoomCameraMaxFps(30)
        model.joinMeetingId = "97682593786"
        model.joinZoom()
        func joins() -> [JSONObject] {
            core.bodies.filter { $0["type"] as? String == "zoom-join" }.compactMap { $0["payload"] as? JSONObject }
        }
        spin(until: { !joins().isEmpty && ShellApp_isInMeeting(model) }, limit: 3)
        expectEqual(joins().first?["cameraMaxResolution"] as? Int, 1, "720p rides the join as SDK tier 1 (a number)")
        expectEqual(joins().first?["cameraMaxFps"] as? Int, 30, "the fps ceiling rides the join")
        expectEqual(model.zoomCameraApplied, ZoomCameraCeiling(tier: 1, fps: 30), "the join that reached the meeting latched it")
        model.setZoomCameraMaxResolution("360p")
        expectEqual(model.zoomCameraApplied.tier, 1, "a mid-meeting edit does not change what is applied")
        expect(model.zoomCameraMaxStatus.contains("360p pending for next Zoom join"), "and the status says so")
    }

    @MainActor
    private static func ShellApp_isInMeeting(_ model: AppModel) -> Bool { AppModel.isInMeeting(model.meetingState) }

    /// Mutation: drop the restore lines -> red.
    @MainActor
    private static func testZ1PrefsRoundTrip() {
        let model = AppModel()
        model.setZoomCameraMaxResolution("360p")
        model.setZoomCameraMaxFps(24)
        let relaunched = AppModel()
        relaunched.applyRestoredPrefs(model.currentPrefs())
        expectEqual(relaunched.zoomCameraMaxResolution, "360p", "resolution ceiling restored")
        expectEqual(relaunched.zoomCameraMaxFps, 24, "fps ceiling restored")
        var junk = ShellPrefs(); junk.zoomCameraMaxResolution = "8K"; junk.zoomCameraMaxFps = 7
        let fresh = AppModel(); fresh.applyRestoredPrefs(junk)
        expectEqual(fresh.zoomCameraMaxResolution, "1080p", "an unknown saved ceiling normalizes to 1080p")
        expectEqual(fresh.zoomCameraMaxFps, 60, "an unknown saved fps normalizes to 60")
    }

    // ── Z2: capture audio-sync nudge ────────────────────────────────────────

    /// The Windows label table and the +/-500 clamp: 26 presses of +20 stop at
    /// 500 (26 x 20 = 520 would pass it). Mutation: drop the clamp -> red.
    @MainActor
    private static func testZ2ClampAndLabel() {
        expectEqual(CaptureAudioSyncPolicy.label(0), "Audio sync 0 ms", "zero")
        expectEqual(CaptureAudioSyncPolicy.label(40), "Audio delayed +40 ms", "later")
        expectEqual(CaptureAudioSyncPolicy.label(-60), "Audio advanced -60 ms", "earlier (Windows keeps the sign)")
        let model = AppModel()
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Cam", kind: "video", vendor: "",
                                                 connectionState: "connected", signalPresent: true, warning: "")]
        for _ in 0..<26 { model.nudgeCaptureAudio(deviceId: "cam1", deltaMs: 20) }
        expectEqual(model.captureAudioSyncOffset("cam1"), 500, "26 presses clamp at +500")
        for _ in 0..<60 { model.nudgeCaptureAudio(deviceId: "cam1", deltaMs: -20) }
        expectEqual(model.captureAudioSyncOffset("cam1"), -500, "and at -500")
        model.resetCaptureAudioSync(deviceId: "cam1")
        expectEqual(model.captureAudioSyncOffset("cam1"), 0, "reset")
        model.nudgeCaptureAudio(deviceId: "nope", deltaMs: 20)
        expectEqual(model.captureAudioSyncOffset("nope"), 0, "an unknown device is ignored")
    }

    /// Both copies: the device command (offsetMs) and the paired mic's sources
    /// entry (audioSyncOffsetMs), which a core restart's reassert carries; a new
    /// core generation re-sends the device copy.
    /// Mutations: leave the offset out of the sources entry -> red (entry and
    /// reassert); skip reassertCaptureAudioSyncOffsets -> red (generation).
    @MainActor
    private static func testZ2BothCopiesReachTheCore() {
        let model = AppModel()
        var bodies: [JSONObject] = []
        model.captureRequestForTesting = { body in await MainActor.run { bodies.append(body) }; return [:] }
        model.captureDevices = [CaptureDeviceRow(id: "cam1", name: "Cam", kind: "video", vendor: "",
                                                 connectionState: "connected", signalPresent: true, warning: "")]
        model.slots[0].kind = "capture"; model.slots[0].sourceId = "cam1"
        model.slots[0].audioDeviceId = "mic-uid"; model.slots[0].audioDeviceName = "USB Mic"
        model.nudgeCaptureAudio(deviceId: "cam1", deltaMs: 20)
        model.nudgeCaptureAudio(deviceId: "cam1", deltaMs: 20)
        spin(until: { bodies.count >= 2 }, limit: 2)
        let device = bodies.last { $0["type"] as? String == "set-capture-audio-sync-offset" }?["payload"] as? JSONObject
        expectEqual(device?["deviceId"] as? String, "cam1", "the device command names the device")
        expectEqual(device?["offsetMs"] as? Int, 40, "and carries the clamped offset")
        expectEqual(model.takeStatus, "Cam audio sync set to 40 ms", "Windows status line")
        let entry = (model.captureAudioSourcesCommand()["sources"] as? [JSONObject])?
            .first { $0["captureDeviceId"] as? String == "cam1" }
        expectEqual(entry?["audioSyncOffsetMs"] as? Int, 40, "the paired mic's entry carries it")
        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.reassertAudioConsole()
        let reasserted = (batches.last?.first { $0["type"] as? String == "sync-capture-audio-sources" }?["sources"]
            as? [JSONObject])?.first { $0["captureDeviceId"] as? String == "cam1" }
        expectEqual(reasserted?["audioSyncOffsetMs"] as? Int, 40, "a respawned core gets the mic copy back")
        bodies.removeAll()
        model.reassertCaptureAudioSyncOffsets()
        spin(until: { !bodies.isEmpty }, limit: 2)
        expectEqual((bodies.first?["payload"] as? JSONObject)?["offsetMs"] as? Int, 40,
                    "a new core generation gets the device copy back")
        let saved = (try? JSONEncoder().encode(model.currentPrefs())).flatMap { String(data: $0, encoding: .utf8) } ?? ""
        expect(!saved.isEmpty && !saved.lowercased().contains("sync"),
               "the offset is session state: nothing about it is persisted (plan D2, Windows)")
    }
}
