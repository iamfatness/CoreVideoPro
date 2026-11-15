// Tier 0 mac-vs-Windows parity tests (2026-11-15 consolidated audit, Tier 0
// table). Kept in their own file so the parallel audio / multiview branches
// that also extend ShellTests.swift do not collide line-for-line.

import AppKit
import Foundation
import SwiftUI

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var tier0Cases: [(String, () -> Void)] {
        [
            ("tier0/production-settings-relaunch", testProductionSettingsSurviveARelaunch),
            ("tier0/production-settings-every-generation",
             testProductionSettingsAreReassertedOnEveryCoreGeneration),
            ("tier0/production-labels-and-bitrate", testProductionLabelsAndStreamBitrateAreReal),
            ("tier0/slots-survive-relaunch", testSlotAssignmentsSurviveARelaunch),
            ("tier0/slot-settings-follow-the-source", testPerSourceSettingsFollowTheSourceNotTheSlot),
            ("tier0/iso-survives-first-assignment", testRestoredIsoSelectionSurvivesTheFirstAssignment),
            ("tier0/iso-matrix-one-truth", testIsoMatrixAndSourcesToggleAreOneTruth),
            ("tier0/sources-rows-fit-the-page", testSourcesRowsFitThePageAtTheMinimumWindow),
            ("tier0/recording-folder-chosen-and-persisted", testRecordingFolderIsChosenPersistedAndUsed),
            ("tier0/recording-folder-falls-back-loudly", testRecordingFolderFallsBackLoudly),
            ("tier0/zoom-status-reads-sdk-readiness", testZoomStatusReadsTheCoresSdkReadiness),
            ("tier0/lower-third-keys-a-program-source", testManualLowerThirdKeysAProgramSourceTheCoreGateAdmits),
            ("tier0/lower-third-status-from-core", testLowerThirdStatusComesFromTheCoresOverlayState),
            ("tier0/lower-third-resolver-is-windows", testLowerThirdResolverIsTheWindowsRule),
        ]
    }

    // ── 0.7 production settings persist + re-assert ─────────────────────────

    /// A 4K / 30 fps / grid show came back as 1080p60 P/P-top on every launch:
    /// none of these values were in ShellPrefs.
    @MainActor
    static func testProductionSettingsSurviveARelaunch() {
        let before = AppModel()
        before.canvasWidth = 3840
        before.canvasHeight = 2160
        before.canvasFps = 30
        before.streamBitrateMbps = 12.5
        before.recordFormat = "mov"
        before.recordPrefix = "gala"
        before.multiviewLayoutMode = "grid"
        before.multiviewTileCount = 6
        guard let data = try? JSONEncoder().encode(before.currentPrefs()),
              let saved = try? JSONDecoder().decode(ShellPrefs.self, from: data) else {
            expect(false, "prefs encode/decode")
            return
        }
        let after = AppModel()
        after.applyRestoredPrefs(saved)
        expectEqual(after.canvasWidth, 3840, "canvas width restored")
        expectEqual(after.canvasHeight, 2160, "canvas height restored")
        expectEqual(after.canvasFps, 30, "canvas fps restored")
        expectEqual(after.streamBitrateMbps, 12.5, "stream bitrate restored")
        expectEqual(after.recordFormat, "mov", "recording format restored")
        expectEqual(after.recordPrefix, "gala", "recording prefix restored")
        expectEqual(after.multiviewLayoutMode, "grid", "multiview layout restored")
        expectEqual(after.multiviewTileCount, 6, "multiview tile count restored")

        // An older file (no production keys) keeps the built-in defaults.
        let older = AppModel()
        older.applyRestoredPrefs(ShellPrefs())
        expectEqual(older.canvasLabel, "1080p60", "absent keys keep the 1080p60 default")
        expectEqual(older.multiviewLayoutMode, "pgmPvwTop", "absent keys keep P/P top")

        // A hand-edited file cannot put an unpickable value on the wire.
        var bad = ShellPrefs()
        bad.canvasWidth = 1234; bad.canvasHeight = 567; bad.canvasFps = 59
        bad.recordFormat = "avi"; bad.multiviewLayoutMode = "nonsense"
        bad.multiviewTileCount = 99; bad.streamBitrateMbps = 500
        let guarded = AppModel()
        guarded.applyRestoredPrefs(bad)
        expectEqual(guarded.canvasWidth, 1920, "an unpickable canvas is refused")
        expectEqual(guarded.canvasFps, 60, "an unpickable fps is refused")
        expectEqual(guarded.recordFormat, "mp4", "an unknown format is refused")
        expectEqual(guarded.multiviewLayoutMode, "pgmPvwTop", "an unknown layout is refused")
        expectEqual(guarded.multiviewTileCount, 16, "tile count clamps to the stepper range")
        expectEqual(guarded.streamBitrateMbps, 20, "bitrate clamps to the slider range")
    }

    /// A respawned core starts at 1920x1080@60 / grid. The canvas and the
    /// multiview config must ride every core generation, not one launch.
    @MainActor
    static func testProductionSettingsAreReassertedOnEveryCoreGeneration() {
        let core = FakeTakeCore()
        let model = AppModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        var saved = ShellPrefs()
        saved.canvasWidth = 3840; saved.canvasHeight = 2160; saved.canvasFps = 30
        saved.multiviewLayoutMode = "grid"; saved.multiviewTileCount = 6
        model.applyRestoredPrefs(saved)
        model.reassertProductionSettings()
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        let commands = (core.bodies.first?["commands"] as? [JSONObject]) ?? []
        let profile = commands.first { $0["type"] as? String == "set-output-profile" }
        let multiview = commands.first { $0["type"] as? String == "configure-multiviewer" }
        expectEqual(profile?["width"] as? Int, 3840, "the restored canvas width is re-sent")
        expectEqual(profile?["fps"] as? Int, 30, "the restored canvas fps is re-sent")
        expectEqual(multiview?["layoutMode"] as? String, "grid", "the restored layout is re-sent")
        expectEqual(multiview?["tileCount"] as? Int, 6, "the restored tile count is re-sent")

        // onConnected runs on EVERY generation (initial handshake and respawn);
        // it must call the re-assert, not the old multiview-only one-shot.
        guard let body = functionBody(named: "private func onConnected()", in: "AppModel.swift") else {
            expect(false, "AppModel.swift onConnected() not found next to the binary")
            return
        }
        expect(body.contains("reassertProductionSettings()"),
               "onConnected re-asserts the production settings on every core generation")
    }

    /// The labels follow the real values; the stream recipe sends the
    /// operator's bitrate rather than a hard-coded 6 Mbps.
    @MainActor
    static func testProductionLabelsAndStreamBitrateAreReal() {
        let model = AppModel()
        model.canvasHeight = 2160
        model.canvasWidth = 3840
        model.canvasFps = 30
        expectEqual(model.canvasLabel, "2160p30", "multiview chip reflects the real canvas")
        model.streamBitrateMbps = 12.5
        let start = model.startProgramOutput(recording: false, streaming: true)
        let profile = start["streamOutputProfile"] as? JSONObject
        expectEqual((profile?["targetBitrateMbps"] as? NSNumber)?.doubleValue, 12.5,
                    "the stream profile carries the operator's bitrate")
        let rtmp = (start["destinationSettings"] as? [JSONObject])?.first
        expectEqual((rtmp?["targetBitrateMbps"] as? NSNumber)?.doubleValue, 12.5,
                    "the RTMP destination's explicit bitrate (which wins core-side) is the operator's")
        expect(ProductionSettingsPolicy.streamRecipeLabel(bitrateMbps: 12.5).contains("12.5 Mbps"),
               "the stream flyout states the bitrate that is sent")
        guard let shell = sourceFile("ShellApp.swift") else {
            expect(false, "ShellApp.swift not found next to the binary")
            return
        }
        expect(!shell.contains("MonoChip(\"1080p60\")"), "no hard-coded 1080p60 multiview chip")
    }

    // ── 0.8 slot assignments + ISO selection persist ────────────────────────

    @MainActor
    static func relaunched(_ before: AppModel) -> AppModel? {
        guard let data = try? JSONEncoder().encode(before.currentPrefs()),
              let saved = try? JSONDecoder().decode(ShellPrefs.self, from: data) else { return nil }
        let after = AppModel()
        after.applyRestoredPrefs(saved)
        return after
    }

    /// Capture rows vanished every launch (sources audit row 75).
    @MainActor
    static func testSlotAssignmentsSurviveARelaunch() {
        let before = AppModel()
        before.assignSlot(3, kind: "capture", sourceId: "cam1", name: "Wide cam")
        before.assignSlot(1, kind: "zoom", sourceId: "42", name: "Guest")
        before.toggleSlotInShow(1)   // parked, not in show
        guard let after = relaunched(before) else { expect(false, "relaunch"); return }
        let cam = after.slots.first { $0.id == 3 }
        expectEqual(cam?.kind, "capture", "capture assignment restored")
        expectEqual(cam?.sourceId, "cam1", "capture source restored")
        expectEqual(cam?.name, "Wide cam", "display name restored")
        expectEqual(cam?.inShow, true, "in-show restored")
        expectEqual(cam?.offline, true, "a restored source reads offline until it is seen")
        let guest = after.slots.first { $0.id == 1 }
        expectEqual(guest?.sourceId, "42", "zoom assignment restored")
        expectEqual(guest?.inShow, false, "a parked slot stays parked")
        expect(after.captureReconnectPending.contains("cam1"),
               "a restored capture slot reconnects its device when the core lists it")
        let empty = AppModel()
        empty.applyRestoredPrefs(ShellPrefs())
        expect(empty.slots.allSatisfy { $0.kind == "unassigned" },
               "an older prefs file (no slots key) starts with empty slots, as before")
    }

    /// Key / mic / dropout were keyed by SLOT NUMBER, so they reattached to
    /// whatever source landed in that slot.
    @MainActor
    static func testPerSourceSettingsFollowTheSourceNotTheSlot() {
        var prefs = ShellPrefs()
        // The camera now sits in slot 5; its settings were saved from slot 3.
        prefs.showInputSlots = [
            PersistedSlot(slotId: 3, kind: "capture", sourceId: "other", name: "Other", inShow: true),
            PersistedSlot(slotId: 5, kind: "capture", sourceId: "cam1", name: "Cam", inShow: true),
            PersistedSlot(slotId: 2, kind: "zoom", sourceId: "42", name: "Guest", inShow: true),
        ]
        prefs.chromaKeys = [PersistedChromaKey(slotId: 3, colorHex: "#0000ff", sourceKey: "capture:cam1")]
        prefs.capturePairings = [CapturePairing(slotId: 3, audioDeviceId: "mic", audioDeviceName: "Mic",
                                                sourceKey: "capture:cam1")]
        prefs.slotDropoutPolicies = [PersistedSlotPolicy(slotId: 7, dropoutPolicy: "black",
                                                         sourceKey: "zoom:42")]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        let cam = model.slots.first { $0.id == 5 }
        let other = model.slots.first { $0.id == 3 }
        expectEqual(cam?.keyEnabled, true, "the key reattaches to its source's slot")
        expectEqual(cam?.keyColorHex, "#0000ff", "with its own settings")
        expectEqual(other?.keyEnabled, false, "never to whatever now sits in the old slot")
        expectEqual(cam?.audioDeviceId, "mic", "the mic pairing follows the camera")
        expectEqual(other?.audioDeviceId, "", "the old slot does not inherit the mic")
        expectEqual(model.slots.first { $0.id == 2 }?.dropoutPolicy, "black",
                    "the dropout policy follows the guest")

        // Saving writes the source identity, so the next restore can do this.
        let saved = model.currentPrefs()
        expectEqual(saved.chromaKeys?.first?.sourceKey, "capture:cam1", "keys save their source")
        expectEqual(saved.capturePairings?.first?.sourceKey, "capture:cam1", "pairings save their source")
        expectEqual(saved.slotDropoutPolicies?.first?.sourceKey, "zoom:42", "policies save their source")
    }

    /// recomputeFromSlots rebuilt the selection from slot flags; a freshly
    /// assigned slot has iso=false, so the first assignment wiped it.
    @MainActor
    static func testRestoredIsoSelectionSurvivesTheFirstAssignment() {
        var prefs = ShellPrefs()
        prefs.isoRecordingEnabled = true
        prefs.isoSelectedSourceIds = ["capture:cam1", "zoom:7"]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        model.assignSlot(2, kind: "zoom", sourceId: "7", name: "Guest")
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "zoom:7"],
                    "the first assignment keeps the restored ISO selection")
        expect(model.isoRecordingEnabled, "and the ISO master switch")
        expectEqual(model.slots.first { $0.id == 2 }?.iso, true,
                    "the assigned slot shows its source's saved ISO state")
        model.unassignSlot(2)
        expect(model.isoSelectedSourceIds.contains("zoom:7"),
               "unassign keeps the source's ISO intent (Windows ShowInputsCoordinator)")
    }

    /// The matrix kept its own column map, never rebuilt from the selection,
    /// so after a relaunch or a Sources-tab toggle it showed no ISO dots while
    /// ISOs were armed — and the next click replaced the whole selection.
    @MainActor
    static func testIsoMatrixAndSourcesToggleAreOneTruth() {
        var prefs = ShellPrefs()
        prefs.isoRecordingEnabled = true
        prefs.isoSelectedSourceIds = ["capture:cam1"]
        prefs.showInputSlots = [
            PersistedSlot(slotId: 1, kind: "capture", sourceId: "cam1", name: "Cam", inShow: true),
            PersistedSlot(slotId: 2, kind: "zoom", sourceId: "7", name: "Guest", inShow: true),
        ]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        expectEqual(model.isoColumn(of: "capture:cam1"), 1, "the matrix shows the restored ISO")
        model.toggleSlotIso(2)      // Sources tab
        expect(model.isoColumn(of: "zoom:7") != nil, "a Sources-tab ISO toggle appears in the matrix")
        model.toggleIsoCell(column: 5, sourceId: "zoom:7")   // move to ISO 5
        expectEqual(model.isoColumn(of: "zoom:7"), 5, "the matrix move keeps its column")
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1", "zoom:7"],
                    "a matrix click edits the selection, it does not replace it")
        model.toggleSlotIso(1)      // Sources tab: cam1 off
        expectEqual(model.isoColumn(of: "capture:cam1"), nil, "unchecking in Sources clears its matrix cell")
        expectEqual(model.slots.first { $0.id == 1 }?.iso, false, "and its Sources checkbox")
        model.toggleIsoCell(column: 5, sourceId: "capture:cam1")  // takes zoom:7's column
        expectEqual(model.isoSelectedSourceIds, ["capture:cam1"], "column-exclusive: the occupant leaves")
        expectEqual(model.slots.first { $0.id == 2 }?.iso, false, "and its Sources checkbox follows")
        expectEqual(model.slots.first { $0.id == 1 }?.iso, true, "the matrix pick shows in Sources")
    }

    // ── 0.10 tab width cap ───────────────────────────────────────────────────

    /// TabPage capped every tab at 720 pt; an assigned Sources row is built
    /// from fixed-width controls of ~1000 pt, so it was compressed or clipped.
    @MainActor
    static func testSourcesRowsFitThePageAtTheMinimumWindow() {
        let model = AppModel()
        TabSnapshotHarness.seed(model)   // zoom + capture-with-mic rows (the widest)
        var widest: CGFloat = 0
        for slot in model.slots where slot.kind != "unassigned" {
            let host = NSHostingView(rootView: SlotRow(slot: slot).environmentObject(model)
                .fixedSize())
            host.layoutSubtreeIfNeeded()
            widest = max(widest, host.fittingSize.width)
        }
        expect(widest > 600, "slot row measured (\(widest) pt)")
        // The Sources card adds 14 pt padding each side inside the page.
        let available = TabPageLayout.pageWidth(for: .sources, window: 1360) - 28
        expect(widest <= available,
               "an assigned Sources row (\(widest) pt) fits the page at the 1360 pt minimum window (\(available) pt)")
        expect(TabPageLayout.pageWidth(for: .routing, window: 1360) >= 760,
               "the Routing grid (~724 pt) fits without clipping its last column")
        for tab in StudioTab.allCases where tab != .studio {
            expect(TabPageLayout.pageWidth(for: tab, window: 1360) >= 720,
                   "\(tab.rawValue) keeps at least the old 720 pt measure")
        }
        guard let shell = sourceFile("ShellApp.swift") else {
            expect(false, "ShellApp.swift not found next to the binary"); return
        }
        expect(!shell.contains(".frame(maxWidth: 720)\n                Spacer(minLength: 0)"),
               "TabPage no longer applies a flat 720 pt cap")
        expect(shell.contains("TabPageContent(tab: model.selectedTab)"),
               "TabPage draws the shared TabPageContent (what the snapshot harness renders)")
    }

    // ── 0.11 recording folder chooser ────────────────────────────────────────

    static func scratchDir(_ name: String) -> String {
        let dir = NSTemporaryDirectory() + "corevideo-shell-harness-\(getpid())/\(name)-\(UUID().uuidString)"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        return dir
    }

    /// The folder was fixed to ~/Movies/CoreVideoPro: no external drive.
    @MainActor
    static func testRecordingFolderIsChosenPersistedAndUsed() {
        let chosen = scratchDir("external")
        let before = AppModel()
        expect(before.setRecordingFolder(chosen), "a writable folder is accepted")
        guard let after = relaunched(before) else { expect(false, "relaunch"); return }
        expectEqual(after.recordingFolderChoice, chosen, "the chosen folder survives a relaunch")
        expectEqual(after.recordingFolderDisplay, chosen, "Settings shows the chosen folder")

        let core = FakeTakeCore()
        after.quitRequestForTesting = { body, _ in try await core.handle(body) }
        after.toggleRecording()
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        let commands = (core.bodies.first?["commands"] as? [JSONObject]) ?? []
        let targets = commands.first { $0["type"] as? String == "set-recording-targets" }
        expectEqual(targets?["targetFolder"] as? String, chosen, "Record writes to the chosen folder")

        // A folder that cannot take recordings is refused, and the choice stands.
        let locked = scratchDir("locked")
        chmod(locked, 0o555)
        defer { chmod(locked, 0o755) }
        expect(!after.setRecordingFolder(locked), "a read-only folder is refused")
        expect(after.recordingFolderWarning.contains("not writable"), "and says why")
        expectEqual(after.recordingFolderChoice, chosen, "the previous choice stands")
    }

    /// An unplugged external drive must not fail a recording silently or
    /// create a stray /Volumes folder: fall back to the default, loudly.
    @MainActor
    static func testRecordingFolderFallsBackLoudly() {
        let missing = "/Volumes/CVP-unplugged-\(UUID().uuidString)/Recordings"
        var prefs = ShellPrefs()
        prefs.recordingFolder = missing
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        let folder = model.recordingTargetFolder()
        expect(folder != missing, "an unplugged drive is not recorded to")
        expect(folder.hasPrefix(NSTemporaryDirectory()), "the harness default folder is used instead")
        expect(model.recordingFolderWarning.contains("is not connected"),
               "Settings says the drive is not connected")
        expect(model.recordingFolderWarning.contains(missing), "and names the folder")
        expect(model.warnings.first?.hasPrefix("recording: Recording folder unavailable") == true,
               "the warning strip says so too")
        expect(!FileManager.default.fileExists(atPath: missing), "nothing is created under /Volumes")
        expectEqual(model.recordingFolderChoice, missing, "the choice is kept for when the drive returns")
    }

    // ── 0.15 Zoom tab status reflects SDK readiness ─────────────────────────

    /// The Zoom tab said "Ready to join Zoom" whatever the SDK's state; a
    /// missing engine or failed SDK showed only on Health.
    @MainActor
    static func testZoomStatusReadsTheCoresSdkReadiness() {
        // The core's exact no-engine node (CoreStateCapture::zoomReadinessState).
        let noEngine: JSONObject = ["zoom": ["readiness": [
            "status": "blocked", "mode": "no-engine", "sdkAvailable": false,
            "checks": [["id": "zoom-engine-runtime", "status": "blocked",
                        "label": "COREVIDEO_ZOOM_ENGINE_PATH is not set: no Zoom engine, so Join is refused"]],
        ] as JSONObject] as JSONObject]
        let ready: JSONObject = ["zoom": ["readiness": ["status": "ready", "mode": "runtime"] as JSONObject]
            as JSONObject]
        let blocked = ZoomOperatorStatus.readiness(snapshot: noEngine)
        expectEqual(blocked, .blocked, "the core's blocked readiness is read")
        expectEqual(ZoomOperatorStatus.chipLabel(blocked, inMeeting: false), "Zoom is unavailable",
                    "Windows wording when blocked")
        expectEqual(ZoomOperatorStatus.guidance(blocked, inMeeting: false),
                    "CoreVideo cannot join Zoom yet. Open Health for details.", "and where to look")
        expect(ZoomOperatorStatus.blockingReason(snapshot: noEngine)?.contains("no Zoom engine") == true,
               "the blocking check is named on the Zoom tab")
        let fine = ZoomOperatorStatus.readiness(snapshot: ready)
        expectEqual(ZoomOperatorStatus.chipLabel(fine, inMeeting: false), "Ready to join Zoom", "ready")
        expectEqual(ZoomOperatorStatus.chipLabel(blocked, inMeeting: true), "Connected to Zoom",
                    "Windows wording in a meeting")
        expectEqual(ZoomOperatorStatus.readiness(snapshot: [:]), .ready,
                    "no snapshot yet is not evidence of a block")
        guard let shell = sourceFile("ShellApp.swift") else {
            expect(false, "ShellApp.swift not found next to the binary"); return
        }
        expect(shell.contains("ZoomOperatorStatus.chipLabel(readiness, inMeeting: inMeeting)"),
               "the Zoom tab's status chip comes from the core's readiness")
        expect(!shell.contains(": \"Ready to join Zoom\")"), "no fixed \"Ready to join Zoom\" chip")
    }

    // ── 0.6 manual lower third ───────────────────────────────────────────────

    /// A rendered Program frame carrying one Zoom guest (the core's
    /// programFrame.videoSources shape, RenderedProgramSources.h).
    @MainActor
    static func seedProgramSource(_ model: AppModel, pid: String = "42", name: String = "Guest") {
        model.programSceneId = "s-prog"
        model.roster = [RosterParticipant(id: pid, name: name, hasVideo: true,
                                          muted: false, talking: false, assigned: true)]
        model.lastSnapshot = ["programFrame": [
            "frameNumber": 120, "sceneId": "s-prog", "renderPlanId": "s-prog:1:0",
            "videoSources": [["kind": "participant-video", "layerId": "route:s-prog-0",
                              "participantId": pid, "sourceId": "zoom:" + pid]],
        ] as JSONObject]
    }

    /// The video kinds and id comparison of the core's "bind lower thirds to
    /// rendered Program" gate, read from MediaCore.cpp (like the payload
    /// contract test reads JsonRpcServer.cpp).
    static func lowerThirdGateFromCoreSource() -> (kinds: Set<String>, comparesIds: Bool)? {
        var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        var source: String?
        for _ in 0..<8 {
            let candidate = dir.appendingPathComponent("native/src/core/MediaCore.cpp")
            if let text = try? String(contentsOf: candidate, encoding: .utf8) { source = text; break }
            dir = dir.deletingLastPathComponent()
        }
        guard let text = source,
              let start = text.range(of: "isLowerThird && !asset->sourceId.empty()") else { return nil }
        let window = String(text[start.lowerBound...].prefix(1500))
        guard let close = window.range(of: "continue;") else { return nil }
        let gate = String(window[..<close.lowerBound])
        var kinds: Set<String> = []
        var rest = gate[...]
        while let hit = rest.range(of: "video.kind == \"") {
            let after = rest[hit.upperBound...]
            guard let end = after.firstIndex(of: "\"") else { break }
            kinds.insert(String(after[..<end]))
            rest = after[end...]
        }
        let compares = gate.contains("video.sourceId == asset->sourceId")
            && gate.contains("video.participantId == asset->sourceId")
        return (kinds, compares)
    }

    /// The Mac keyed sourceId "mac:manual", which no Program layer ever has, so
    /// the core's on-Program gate dropped the key while the tab said on air.
    @MainActor
    static func testManualLowerThirdKeysAProgramSourceTheCoreGateAdmits() {
        guard let gate = lowerThirdGateFromCoreSource() else {
            expect(false, "the core's lower-third gate (MediaCore.cpp) was not found — re-read the contract")
            return
        }
        expect(gate.comparesIds, "the gate admits a key whose sourceId is a Program layer's sourceId/participantId")
        expect(ProgramLowerThirdSource.subjectKinds.isSubset(of: gate.kinds),
               "every kind the shell can key is one the gate admits (gate: \(gate.kinds.sorted()))")

        let model = AppModel()
        seedProgramSource(model, pid: "42", name: "Jordan")
        model.assignSlot(1, kind: "zoom", sourceId: "42", name: "Jordan Wallace")
        var sent: [JSONObject] = []
        model.overlayCommandObserver = { sent.append($0) }
        model.showLowerThird()
        let key = sent.first { $0["keyPhase"] as? String == "building-in" }
        let keyed = key?["sourceId"] as? String ?? ""
        expect(keyed != "mac:manual", "the key never carries the synthetic mac:manual id")
        let programIds = ((model.lastSnapshot["programFrame"] as? JSONObject)?["videoSources"]
                          as? [JSONObject] ?? []).compactMap { $0["sourceId"] as? String }
        expect(programIds.contains(keyed), "the key names a source on rendered Program (\(keyed))")
        expectEqual(key?["sourceName"] as? String, "Jordan Wallace", "it keys the Sources display name")
        expectEqual(key?["text"] as? String, "Jordan Wallace", "as the keyed text")

        // Nothing on Program: refuse with the Windows wording, send nothing.
        let empty = AppModel()
        var none: [JSONObject] = []
        empty.overlayCommandObserver = { none.append($0) }
        empty.showLowerThird()
        expect(none.isEmpty, "no source on Program sends no key")
        expectEqual(empty.lowerThirdCommandStatus, "Lower third needs a program source", "and says so")

        // Key out keeps the keyed id, so the build-out animates instead of cutting.
        model.hideLowerThird()
        let out = sent.first { $0["keyPhase"] as? String == "building-out" }
        expectEqual(out?["sourceId"] as? String, keyed, "the build-out carries the keyed source id")
    }

    /// "Key on air" came from the requested phase; it must come from the
    /// core's overlayState visible flag (the gate can suppress a key).
    @MainActor
    static func testLowerThirdStatusComesFromTheCoresOverlayState() {
        func overlay(_ phase: String, visible: Bool) -> JSONObject {
            ["overlayState": ["onAirCount": 1, "overlays": [[
                "overlayId": "key:lower-third", "keyPhase": phase, "visible": visible,
                "sourceId": "zoom:42", "sourceName": "Jordan Wallace"]]] as JSONObject]
        }
        let model = AppModel()
        model.applySnapshotForTesting(overlay("on-air", visible: false))
        expectEqual(model.lowerThirdOnAirCount, 0, "a suppressed key is not counted on air")
        expectEqual(model.lowerThirdKeyStatus, LowerThirdKeyReadout.syncPending,
                    "a requested-but-undrawn key says the core has not confirmed it")
        model.applySnapshotForTesting(overlay("on-air", visible: true))
        expectEqual(model.lowerThirdOnAirCount, 1, "a drawn key is on air")
        expectEqual(model.lowerThirdKeyStatus, "Key on air — Jordan Wallace", "and names its source")
        model.applySnapshotForTesting(["overlayState": ["overlays": []] as JSONObject])
        expectEqual(model.lowerThirdKeyStatus, "Key off — no source keyed", "no key: off")
        guard let overlays = sourceFile("Overlays.swift") else {
            expect(false, "Overlays.swift not found next to the binary"); return
        }
        expect(overlays.contains("Text(model.lowerThirdKeyStatus)"), "the keyer status line reads the core's state")
        expect(!overlays.contains("Text(onAir ? \"Key on air\""), "no request-derived \"Key on air\"")
    }

    /// Port of Windows RenderedLowerThirdSourceResolver: rendered layers only,
    /// incoming side of a take, camera-on guests, sticky first.
    @MainActor
    static func testLowerThirdResolverIsTheWindowsRule() {
        func frame(_ sources: [JSONObject], plan: String = "p:1:0", scene: String = "s") -> JSONObject {
            ["frameNumber": 9, "sceneId": scene, "renderPlanId": plan, "videoSources": sources]
        }
        let a: JSONObject = ["kind": "participant-video", "layerId": "route:a", "participantId": "1", "sourceId": "zoom:1"]
        let b: JSONObject = ["kind": "participant-video", "layerId": "take-in:b", "participantId": "2", "sourceId": "zoom:2"]
        let cam: JSONObject = ["kind": "participant-video", "layerId": "route:c", "participantId": "", "sourceId": "capture:cam"]
        func pick(_ f: JSONObject, sticky: String = "", video: Set<String> = ["1", "2"],
                  devices: Set<String> = ["cam"], scene: String = "s") -> String? {
            ProgramLowerThirdSource.resolve(programFrame: f, programSceneId: scene, stickySourceId: sticky,
                                            rosterWithVideo: video, captureDeviceIds: devices)?.sourceId
        }
        expectEqual(pick(frame([a, b])), "zoom:1", "first rendered guest")
        expectEqual(pick(frame([a, b]), sticky: "zoom:2"), "zoom:2", "sticky wins while still on Program")
        expectEqual(pick(frame([a, b], plan: "take:x")), "zoom:2", "a transition keys the incoming side")
        expectEqual(pick(frame([a]), video: []), nil, "a camera-off guest is not a subject")
        expectEqual(pick(frame([cam])), "capture:cam", "a connected camera is a subject")
        expectEqual(pick(frame([cam]), devices: []), nil, "an unknown capture device is not")
        expectEqual(pick(frame([a], scene: "other")), nil, "a frame from another scene is not trusted")
    }

    // ── source-reading helpers ───────────────────────────────────────────────

    static func sourceFile(_ name: String) -> String? {
        let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        return try? String(contentsOf: dir.appendingPathComponent(name), encoding: .utf8)
    }

    /// The text of one function: from its signature to the next line that
    /// closes a member at the declaration's indentation.
    static func functionBody(named signature: String, in file: String) -> String? {
        guard let source = sourceFile(file), let start = source.range(of: signature) else {
            return nil
        }
        let lineStart = source[..<start.lowerBound].lastIndex(of: "\n")
            .map { source.index(after: $0) } ?? source.startIndex
        let indent = String(source[lineStart..<start.lowerBound])
        let tail = source[start.upperBound...]
        guard let end = tail.range(of: "\n" + indent + "}\n") else { return String(tail) }
        return String(tail[..<end.upperBound])
    }
}
