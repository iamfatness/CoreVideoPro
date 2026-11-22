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
            ("quickwins/a5-first-connect-sends-the-saved-source", testA5FirstConnectSendsTheSavedSource),
            ("quickwins/a5-hot-plugged-mic-goes-live", testA5HotPluggedMicGoesLive),
            ("quickwins/z2-device-copy-reconciles-from-the-snapshot", testZ2DeviceCopyReconcilesFromTheSnapshot),
            ("quickwins/z1-windows-preference-table", testZ1WindowsPreferenceTable),
            ("quickwins/z1-join-carries-and-latches-the-ceiling", testZ1JoinCarriesAndLatchesTheCeiling),
            ("quickwins/z1-prefs-round-trip", testZ1PrefsRoundTrip),
            ("quickwins/z2-clamp-and-label", testZ2ClampAndLabel),
            ("quickwins/z2-both-copies-reach-the-core", testZ2BothCopiesReachTheCore),
            ("quickwins/o1-payload-is-the-windows-shape", testO1PayloadIsTheWindowsShape),
            ("quickwins/o1-recording-start-carries-the-profile", testO1RecordingStartCarriesTheProfile),
            ("quickwins/o1-destination-row-every-field", testO1DestinationRowCarriesEveryEncoderField),
            ("quickwins/o1-sanitize-prefs-and-recipe", testO1SanitizePrefsAndRecipe),
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
        expect(CaptureAudioSyncPolicy.offersNudge(category: "Cameras"), "a camera row offers the nudge")
        expect(!CaptureAudioSyncPolicy.offersNudge(category: "Screens"), "a screen row does not (no audio on Mac)")
        model.nudgeCaptureAudio(deviceId: "nope", deltaMs: 20)
        expectEqual(model.captureAudioSyncOffset("nope"), 0, "an unknown device is ignored")
    }

    /// Both copies: the device command (offsetMs) and the paired mic's sources
    /// entry (audioSyncOffsetMs), which a core restart's reassert carries; a new
    /// core generation re-sends the device copy.
    /// Mutations: leave the offset out of the sources entry -> red (entry and
    /// reassert). The device copy on a new core is covered by the reconcile test.
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
        // The device copy on a new core generation is reconciled from the snapshot
        // (z2-device-copy-reconciles-from-the-snapshot).
        let saved = (try? JSONEncoder().encode(model.currentPrefs())).flatMap { String(data: $0, encoding: .utf8) } ?? ""
        expect(!saved.isEmpty && !saved.lowercased().contains("sync"),
               "the offset is session state: nothing about it is persisted (plan D2, Windows)")
    }

    // ── O1: output profiles and advanced stream settings ────────────────────
    // Fixture invariant: every operator value below differs from its default,
    // so a field the code drops reads back as the default and fails.

    private static let o1Profile = RecordingProfile(width: 1280, height: 720, codec: "h265",
                                                    targetBitrateMbps: 12, audioBitrateKbps: 256)
    private static let o1Encoder = StreamEncoderSettings(audioBitrateKbps: 128, keyframeIntervalSeconds: 4,
                                                         rateControl: "vbr", h264Profile: "main", bFrames: 0)

    /// Keys exactly = Windows MediaCoreCommandBuilder.OutputProfilePayload
    /// (:850-860), values from the fixture. Defaults = Windows
    /// DefaultRecordingOutputProfile. Mutation: drop a key -> red (set compare).
    private static func testO1PayloadIsTheWindowsShape() {
        let payload = o1Profile.payload(fps: 50)
        expectEqual(Set(payload.keys), ["profileId", "resolution", "width", "height", "fps",
                                        "targetBitrateMbps", "audioBitrateKbps", "codec"],
                    "exactly the Windows payload keys")
        expectEqual(payload["resolution"] as? String, "1280x720", "resolution")
        expectEqual(payload["fps"] as? Int, 50, "fps")
        expectEqual(payload["codec"] as? String, "h265", "codec")
        expectEqual(payload["targetBitrateMbps"] as? Double, 12, "bitrate")
        expectEqual(payload["audioBitrateKbps"] as? Int, 256, "audio kbps")
        expectEqual(RecordingProfile(), RecordingProfile(width: 1920, height: 1080, codec: "h264",
                                                         targetBitrateMbps: 8, audioBitrateKbps: 192),
                    "defaults = Windows DefaultRecordingOutputProfile (8.2 Mbps, which the core writes as 8)")
    }

    /// A Record start carries the profile on start-program-output AND on
    /// set-recording-targets (renderProfile + the two bitrates).
    /// Mutation: omit renderProfile -> red.
    @MainActor
    private static func testO1RecordingStartCarriesTheProfile() {
        let model = AppModel()
        model.recordingProfile = o1Profile
        let core = FakeTakeCore()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.toggleRecording()
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        let commands = (core.bodies.first?["commands"] as? [JSONObject]) ?? []
        let start = commands.first { $0["type"] as? String == "start-program-output" }
        let targets = commands.first { $0["type"] as? String == "set-recording-targets" }
        expectEqual((start?["recordingOutputProfile"] as? JSONObject)?["width"] as? Int, 1280,
                    "start-program-output carries recordingOutputProfile")
        expectEqual((targets?["renderProfile"] as? JSONObject)?["height"] as? Int, 720, "renderProfile on the targets")
        expectEqual((targets?["renderProfile"] as? JSONObject)?["codec"] as? String, "h265", "with the codec")
        expectEqual(targets?["targetBitrateMbps"] as? Double, 12, "targets bitrate")
        // Review #4: no writer decimates (Windows MF nor Mac AVF: fps is metadata),
        // so the recording rate IS the render rate. The profile says so, which keeps
        // the published recordingFps true.
        expectEqual((targets?["renderProfile"] as? JSONObject)?["fps"] as? Int, model.canvasFps,
                    "the recording profile's fps is the canvas rate")
        expectEqual(targets?["audioBitrateKbps"] as? Int, 256, "targets audio kbps")
        let streamOnly = model.startProgramOutput(recording: false, streaming: true)
        expect(streamOnly["recordingOutputProfile"] == nil, "a stream-only start does not carry the recording profile")
    }

    /// Every field StreamEncoderSettings owns reaches the RTMP and HLS destination
    /// rows the streaming lane's StreamDestinationPolicy builds, driven through the
    /// real start command, one field at a time. The SRT row carries none of them,
    /// as on Windows (StudioViewModel.StreamDestinations.cs).
    /// Mutations: apply(to:) skipping bFrames -> red on that field; applying to
    /// every row -> red on SRT; applying to none -> red on RTMP and HLS.
    @MainActor
    private static func testO1DestinationRowCarriesEveryEncoderField() {
        let model = AppModel()
        model.streamEncoder = o1Encoder
        model.streamKey = "rtmp-key-1"
        model.streamDestinations.srtEnabled = true
        model.streamDestinations.srtHost = "receiver.example.com"
        model.streamDestinations.srtKeyLength = "16"
        model.srtPassphrase = "srt-passphrase-1"
        model.streamDestinations.hlsEnabled = true
        model.streamDestinations.hlsPlaylistUrl = "https://ingest.example.com/live/playlist.m3u8"
        let rows = model.startProgramOutput(recording: false, streaming: true)["destinationSettings"]
            as? [JSONObject] ?? []
        func row(_ id: String) -> JSONObject { rows.first { $0["id"] as? String == id } ?? [:] }
        expectEqual(rows.map { $0["id"] as? String ?? "" }, ["rtmp", "srt", "hls"], "every valid destination has a row")
        let expected: [String: String] = ["audioBitrateKbps": "128", "keyframeIntervalSeconds": "4.0",
                                          "rateControl": "vbr", "h264Profile": "main", "bFrames": "0"]
        expectEqual(Set(expected.keys), Set(StreamEncoderSettings.ownedKeys), "the fixture covers every owned key")
        for id in ["rtmp", "hls"] {
            for key in StreamEncoderSettings.ownedKeys {
                expectEqual(row(id)[key].map { "\($0)" }, expected[key], "the \(id) row carries \(key)")
            }
            expectEqual(row(id)["targetBitrateMbps"] as? Double, model.streamBitrateMbps,
                        "the \(id) bitrate is still the operator's")
            expectEqual(row(id)["fps"] as? Int, 30, "the \(id) stream keeps the destination's 30 fps (plan D3)")
        }
        for key in StreamEncoderSettings.ownedKeys {
            expect(row("srt")[key] == nil, "the srt row carries no \(key), as on Windows")
        }
    }

    /// Garbage is sanitized field by field; prefs round-trip; the footer recipe
    /// is built from the live values. Mutations: recipe literal -> red; drop
    /// the profile restore -> red.
    @MainActor
    private static func testO1SanitizePrefsAndRecipe() {
        let junk = RecordingProfile(width: 999, height: 3, codec: "av1",
                                    targetBitrateMbps: .infinity, audioBitrateKbps: 9000)
            .sanitized(fallback: RecordingProfile())
        expectEqual(junk, RecordingProfile(width: 1920, height: 1080, codec: "h264",
                                           targetBitrateMbps: 8, audioBitrateKbps: 512),
                    "an unpickable size/fps/codec falls back, kbps clamps, a non-finite bitrate falls back")
        expectEqual(StreamEncoderSettings(audioBitrateKbps: 1, keyframeIntervalSeconds: 99, rateControl: "abr",
                                          h264Profile: "ultra", bFrames: 9).sanitized(),
                    StreamEncoderSettings(audioBitrateKbps: 32, keyframeIntervalSeconds: 10, rateControl: "cbr",
                                          h264Profile: "high", bFrames: 4), "encoder clamps and falls back")
        expectEqual(RecordingProfile(targetBitrateMbps: 12.5).sanitized(fallback: RecordingProfile()).targetBitrateMbps,
                    12, "review #4: whole Mbps, because the core's RecordingSessionRequest truncates to int")
        let before = AppModel()
        before.recordingProfile = o1Profile
        before.streamEncoder = o1Encoder
        let after = AppModel()
        after.applyRestoredPrefs(before.currentPrefs())
        expectEqual(after.recordingProfile, o1Profile, "the recording profile survives a relaunch")
        expectEqual(after.streamEncoder, o1Encoder, "the encoder settings survive a relaunch")
        let recipe = ProductionSettingsPolicy.streamRecipeLabel(bitrateMbps: 6, height: 1080, encoder: o1Encoder)
        expect(recipe.contains("VBR") && recipe.contains("AAC 128 kbps"), "the recipe reads the live settings: \(recipe)")
    }

    // ── review round (2026-11-22) ───────────────────────────────────────────

    /// Review #2: the FIRST core connect of a launch must send a saved local
    /// source. onConnected used to reassert the console before listing the audio
    /// inputs, so the device was unknown, nothing was sent, and the mic stayed
    /// silent while the toggle read on (a respawn worked, which hid it).
    /// Drives the real onConnected ordering on a model that has never listed
    /// devices. Mutation: reassert before listing -> red.
    @MainActor
    private static func testA5FirstConnectSendsTheSavedSource() {
        let model = AppModel()
        model.applyRestoredPrefs({ var p = ShellPrefs(); p.localAudioEnabled = true
            p.localAudioDeviceId = "mic-uid"; p.localAudioDeviceName = "USB Mic"; return p }())
        expect(model.audioInputs.isEmpty, "precondition: a fresh launch has listed no devices")
        model.audioInputsProvider = { [a5Mic] }
        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.onConnected()
        let first = batches.first ?? []
        let sources = first.first { $0["type"] as? String == "sync-capture-audio-sources" }
        expect(sources.flatMap(a5Local) != nil, "the first connect's batch carries the saved local source")
        let routing = first.first { $0["type"] as? String == "sync-audio-routing-matrix" }
        expectEqual(a1Buses(routing?["sends"] as? [JSONObject] ?? [], "local-machine-audio"), a1Buses,
                    "with its five default sends")
        let channels = first.first { $0["type"] as? String == "sync-participant-audio-mix" }?["channels"] as? [JSONObject] ?? []
        expect(channels.contains { $0["participantId"] as? String == "local-machine-audio" },
               "and its strip rides the same first batch (FADER LAW)")
    }

    /// Review #2b: a saved mic plugged in AFTER launch becomes live — the device
    /// list is refreshed on connect/disconnect and the console re-pushed when the
    /// local source's sendability changes. Mutation: refresh without the push -> red.
    @MainActor
    private static func testA5HotPluggedMicGoesLive() {
        let model = AppModel()
        model.localAudio = LocalAudioSetting(enabled: true, deviceId: "mic-uid", deviceName: "USB Mic")
        model.audioInputsProvider = { [] }
        model.refreshAudioInputs(push: false)
        var sent: [JSONObject] = []
        model.routingBaseSendObserver = { sent.append($0) }
        model.audioInputsProvider = { [a5Mic] }     // the mic is plugged in
        model.refreshAudioInputs(push: true)
        spin(until: { !sent.isEmpty }, limit: 2)
        let sends = (sent.last?["commands"] as? [JSONObject])?.first?["sends"] as? [JSONObject] ?? []
        expectEqual(a1Buses(sends, "local-machine-audio"), a1Buses, "the plugged-in mic is routed")
        expect(model.strips.contains { $0.id == "local-machine-audio" }, "and has its strip")
        expectEqual(model.localAudioStatus, "Waiting for audio", "the status leaves 'not connected'")
    }

    /// Review #5: on a fresh core the AVF device list fills asynchronously and
    /// set-capture-audio-sync-offset for an unlisted device is dropped silently
    /// (AvfCaptureDeviceAdapter setAudioSyncOffset). The shell reconciles against
    /// what the core REPORTS per device: a listed device whose audioSyncOffsetMs
    /// differs from the operator's gets the command again, once while in flight,
    /// and nothing once they agree. Mutation: skip the reconcile -> red.
    @MainActor
    private static func testZ2DeviceCopyReconcilesFromTheSnapshot() {
        let model = AppModel()
        var bodies: [JSONObject] = []
        var release: CheckedContinuation<Void, Never>?
        model.captureRequestForTesting = { body in
            await MainActor.run { bodies.append(body) }
            await withCheckedContinuation { release = $0 }   // the reply is held: in flight
            return [:]
        }
        model.captureAudioSyncOffsets = ["cam1": 40]
        func device(offset: Int) -> JSONObject {
            ["id": "cam1", "name": "Cam", "kind": "video", "vendor": "avfoundation",
             "connectionState": "connected", "signalPresent": true, "warning": "", "audioSyncOffsetMs": offset]
        }
        func offsetCommands() -> [JSONObject] { bodies.filter { $0["type"] as? String == "set-capture-audio-sync-offset" } }
        model.applySnapshotForTesting(["captureDevices": [] as [JSONObject]])
        spin(until: { false }, limit: 0.1)
        expectEqual(offsetCommands().count, 0, "an unlisted device is not commanded (the core would drop it)")
        model.applySnapshotForTesting(["captureDevices": [device(offset: 0)]])
        spin(until: { !offsetCommands().isEmpty }, limit: 2)
        expectEqual((offsetCommands().first?["payload"] as? JSONObject)?["offsetMs"] as? Int, 40,
                    "the device appears with 0 -> the operator's 40 is sent")
        model.applySnapshotForTesting(["captureDevices": [device(offset: 0)]])
        spin(until: { false }, limit: 0.1)
        expectEqual(offsetCommands().count, 1, "not repeated while that request is in flight")
        release?.resume(); release = nil
        spin(until: { false }, limit: 0.1)
        model.applySnapshotForTesting(["captureDevices": [device(offset: 40)]])
        spin(until: { false }, limit: 0.1)
        expectEqual(offsetCommands().count, 1, "nothing once the core reports the operator's value")
    }
}
